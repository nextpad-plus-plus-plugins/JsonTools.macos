// JsonTabularize.cpp — see JsonTabularize.h.

#include "JsonTabularize.h"

#include <algorithm>
#include <unordered_set>

#include "JsonParser.h"

namespace jsontools {

static Dtype TabSchemaType(const JNodePtr& schema, bool& hasType) {
    JObject* obj = AsObj(schema);
    JNodePtr typeNode;
    hasType = obj && obj->TryGetValue("type", typeNode) && typeNode->type == DT_INT;
    return hasType ? static_cast<Dtype>(typeNode->GetInt()) : DT_TYPELESS;
}

JsonFormat JsonTabularizer::ClassifySchema(const JNodePtr& schema) {
    bool hasType;
    Dtype tipe = TabSchemaType(schema, hasType);
    if ((tipe & DT_SCALAR) != 0) return JF_SCAL;
    JObject* schemaObj = AsObj(schema);
    if (tipe == DT_ARR) {
        JNodePtr itemsNode;
        if (!schemaObj->TryGetValue("items", itemsNode)) return JF_ROW; // empty array is a row
        bool itemHasType;
        Dtype itemDtype = TabSchemaType(itemsNode, itemHasType);
        if (!itemHasType) return JF_BAD; // anyOf: mix of scalars and non-scalars
        if ((itemDtype & DT_SCALAR) != 0) return JF_ROW;
        if (itemDtype == DT_ARR) {
            JNodePtr subitemsNode;
            if (!AsObj(itemsNode)->TryGetValue("items", subitemsNode)) return JF_BAD;
            bool subarrHasType;
            Dtype subarrTypes = TabSchemaType(subitemsNode, subarrHasType);
            if (!subarrHasType) return JF_BAD;
            if ((subarrTypes & DT_ITERABLE) == 0) return JF_SQR;
            return JF_BAD;
        }
        JNodePtr subpropsNode;
        if (!AsObj(itemsNode)->TryGetValue("properties", subpropsNode)) return JF_BAD;
        for (auto& kv : AsObj(subpropsNode)->children) {
            bool hasSubtipe;
            Dtype subtipe = TabSchemaType(kv.second, hasSubtipe);
            if (!hasSubtipe) return JF_BAD;
            if ((subtipe & DT_ITERABLE) != 0) return JF_BAD;
        }
        return JF_REC;
    }
    JNodePtr propsNode;
    if (!schemaObj || !schemaObj->TryGetValue("properties", propsNode)) return JF_BAD;
    JsonFormat cls = JF_ROW;
    for (auto& kv : AsObj(propsNode)->children) {
        bool hasSubtipe;
        Dtype subtipe = TabSchemaType(kv.second, hasSubtipe);
        if (!hasSubtipe) return JF_BAD;
        if (subtipe == DT_OBJ) {
            JNodePtr subpropsNode;
            if (AsObj(kv.second)->TryGetValue("properties", subpropsNode)) {
                for (auto& subkv : AsObj(subpropsNode)->children) {
                    bool subpropHasType;
                    Dtype subproptipe = TabSchemaType(subkv.second, subpropHasType);
                    if (!subpropHasType) return JF_BAD;
                    if ((subproptipe & DT_ARR_OR_OBJ) != 0) return JF_BAD;
                }
            }
        }
        if (subtipe != DT_ARR) continue;
        JNodePtr subitemsNode;
        if (!AsObj(kv.second)->TryGetValue("items", subitemsNode)) continue; // empty arrays are fine
        bool subarrHasType;
        Dtype subarrTipe = TabSchemaType(subitemsNode, subarrHasType);
        if (!subarrHasType) return JF_BAD;
        if ((subarrTipe & DT_ARR_OR_OBJ) != 0) return JF_BAD;
        cls = JF_TAB;
    }
    return cls;
}

void JsonTabularizer::FindTabsInSchemaHelper(
    const JNodePtr& schema, std::vector<JNodePtr>& path,
    std::vector<std::pair<JNodePtr, JsonFormat>>& tabPaths) {
    JsonFormat cls = ClassifySchema(schema);
    if (cls == JF_SCAL || cls == JF_ROW) return;
    if ((cls & JF_ANY_TABLE) != 0) {
        auto patharr = std::make_shared<JArray>();
        patharr->children = path;
        // dedupe by stringified path (the C# used a dict keyed by path string)
        std::string pathstr = patharr->ToString();
        for (auto& existing : tabPaths) {
            if (existing.first->ToString() == pathstr) {
                existing.second = cls;
                return;
            }
        }
        tabPaths.emplace_back(patharr, cls);
        return;
    }
    bool hasType;
    Dtype tipe = TabSchemaType(schema, hasType);
    if (tipe == DT_ARR) {
        JNodePtr itemsNode;
        if (!AsObj(schema)->TryGetValue("items", itemsNode)) return;
        path.push_back(JNode::Int(0)); // 0 = placeholder for all array indices
        JNodePtr anyofNode;
        if (AsObj(itemsNode) && AsObj(itemsNode)->TryGetValue("anyOf", anyofNode)) {
            for (JNodePtr& subschema : AsArr(anyofNode)->children)
                FindTabsInSchemaHelper(subschema, path, tabPaths);
        } else {
            FindTabsInSchemaHelper(itemsNode, path, tabPaths);
        }
        path.pop_back();
        return;
    }
    JNodePtr propsNode;
    if (!AsObj(schema) || !AsObj(schema)->TryGetValue("properties", propsNode)) return;
    for (auto& kv : AsObj(propsNode)->children) {
        path.push_back(JNode::Str(kv.first));
        FindTabsInSchemaHelper(kv.second, path, tabPaths);
        path.pop_back();
    }
}

std::vector<std::pair<JNodePtr, JsonFormat>> JsonTabularizer::FindTabsInSchema(
    const JNodePtr& schema) {
    std::vector<std::pair<JNodePtr, JsonFormat>> tabPaths;
    std::vector<JNodePtr> path;
    FindTabsInSchemaHelper(schema, path, tabPaths);
    return tabPaths;
}

OrderedMap JsonTabularizer::ResolveHang(const OrderedMap& hangingRow, const std::string& keySep) {
    OrderedMap result;
    for (auto& kv : hangingRow) {
        if (JObject* dv = AsObj(kv.second)) {
            for (auto& subkv : dv->children) {
                std::string newKey = kv.first + keySep + subkv.first;
                if (result.contains(newKey))
                    throw JsonToolsError("Attempted to create hanging row with synthetic key " +
                                         newKey + ", but " + newKey + " was already a key");
                result.set(newKey, subkv.second);
            }
        } else {
            result.set(kv.first, kv.second);
        }
    }
    return result;
}

static std::string TabFormatKey(const std::string& key, const std::string& superKey,
                                const std::string& keySep) {
    return superKey.empty() ? key : superKey + keySep + key;
}

static std::string JoinKeys(const std::vector<std::string>& keylist, const std::string& keySep) {
    std::string out;
    for (size_t i = 0; i < keylist.size(); i++) {
        out += keylist[i];
        if (i < keylist.size() - 1) out += keySep;
    }
    return out;
}

void JsonTabularizer::AnyTableToRecord(const JNodePtr& obj, JsonFormat cls,
                                       const OrderedMap& restOfRow,
                                       std::vector<JNodePtr>& result,
                                       std::vector<std::string>& superKeylist,
                                       const std::string& keySep) {
    std::string superKey = JoinKeys(superKeylist, keySep);
    auto keyformat = [&](const std::string& x) { return TabFormatKey(x, superKey, keySep); };
    if (cls == JF_REC) {
        JArray* aobj = AsArr(obj);
        for (JNodePtr& rec : aobj->children) {
            auto newrec = std::make_shared<JObject>();
            for (auto& kv : restOfRow) newrec->children.set(kv.first, kv.second);
            JObject* orec = AsObj(rec);
            for (auto& kv : orec->children) newrec->children.set(keyformat(kv.first), kv.second);
            result.push_back(newrec);
        }
    } else if (cls == JF_TAB) {
        JObject* oobj = AsObj(obj);
        OrderedMap arrDict;   // key → JArray
        OrderedMap notArrDict;
        size_t lenArr = 0;
        for (auto& kv : oobj->children) {
            if (JArray* varr = AsArr(kv.second)) {
                arrDict.set(kv.first, kv.second);
                if (varr->Length() > lenArr) lenArr = varr->Length();
            } else {
                notArrDict.set(keyformat(kv.first), kv.second);
            }
        }
        notArrDict = ResolveHang(notArrDict, keySep);
        if (lenArr == 0) {
            auto row = std::make_shared<JObject>();
            for (auto& kv : notArrDict) row->children.set(kv.first, kv.second);
            for (auto& kv : arrDict) row->children.set(kv.first, JNode::Str(""));
            result.push_back(row);
            return;
        }
        for (size_t ii = 0; ii < lenArr; ii++) {
            auto newrec = std::make_shared<JObject>();
            for (auto& kv : restOfRow) newrec->children.set(kv.first, kv.second);
            for (auto& kv : arrDict) {
                JArray* v = AsArr(kv.second);
                newrec->children.set(keyformat(kv.first),
                                     ii >= v->Length() ? JNode::Str("") : v->children[ii]);
            }
            for (auto& kv : notArrDict) newrec->children.set(kv.first, kv.second);
            result.push_back(newrec);
        }
    } else if (cls == JF_SQR) {
        JArray* aobj = AsArr(obj);
        for (JNodePtr& arr : aobj->children) {
            auto newrec = std::make_shared<JObject>();
            for (auto& kv : restOfRow) newrec->children.set(kv.first, kv.second);
            JArray* achild = AsArr(arr);
            for (size_t ii = 0; ii < achild->Length(); ii++)
                newrec->children.set(keyformat("col" + std::to_string(ii + 1)),
                                     achild->children[ii]);
            result.push_back(newrec);
        }
    }
}

void JsonTabularizer::BuildTableHelper_DEFAULT(const JNodePtr& json, JsonFormat cls, size_t depth,
                                               JArray* tabPath, std::vector<JNodePtr>& result,
                                               const OrderedMap& restOfRow,
                                               std::vector<std::string>& superKeylist,
                                               const std::string& keySep) {
    OrderedMap newRestOfRow;
    for (auto& kv : restOfRow) newRestOfRow.set(kv.first, kv.second);
    if (depth == tabPath->Length()) {
        AnyTableToRecord(json, cls, newRestOfRow, result, superKeylist, keySep);
        return;
    }
    if (JArray* arr = AsArr(json)) {
        for (JNodePtr& subobj : arr->children)
            BuildTableHelper_DEFAULT(subobj, cls, depth + 1, tabPath, result, newRestOfRow,
                                     superKeylist, keySep);
        return;
    }
    std::string newKey = tabPath->children[depth]->GetStr();
    std::string superKey = JoinKeys(superKeylist, keySep);
    JObject* jobj = AsObj(json);
    for (auto& kv : jobj->children) {
        if (kv.first == newKey) continue;
        std::string newk = TabFormatKey(kv.first, superKey, keySep);
        if (JObject* ov = AsObj(kv.second)) {
            for (auto& subkv : ov->children) {
                if ((subkv.second->type & DT_ARR_OR_OBJ) == 0)
                    newRestOfRow.set(newk + keySep + subkv.first, subkv.second);
            }
        } else if (kv.second->type != DT_ARR) {
            newRestOfRow.set(newk, kv.second);
        }
    }
    superKeylist.push_back(newKey);
    BuildTableHelper_DEFAULT(jobj->at(newKey), cls, depth + 1, tabPath, result, newRestOfRow,
                             superKeylist, keySep);
    superKeylist.pop_back();
}

void JsonTabularizer::FlattenSingleRow_FULL_RECURSIVE(const JNodePtr& row,
                                                      const std::string& keySep,
                                                      OrderedMap& restOfRow,
                                                      std::vector<std::string>& superKeylist) {
    std::string superKey = JoinKeys(superKeylist, keySep);
    auto keyformat = [&](const std::string& x) { return TabFormatKey(x, superKey, keySep); };
    if (JArray* arow = AsArr(row)) {
        for (size_t ii = 0; ii < arow->Length(); ii++) {
            JNodePtr child = arow->children[ii];
            std::string colname = "col" + std::to_string(ii + 1);
            if ((child->type & DT_ARR_OR_OBJ) != 0) {
                superKeylist.push_back(colname);
                FlattenSingleRow_FULL_RECURSIVE(child, keySep, restOfRow, superKeylist);
                superKeylist.pop_back();
            } else {
                restOfRow.set(keyformat(colname), child);
            }
        }
    } else if (JObject* orow = AsObj(row)) {
        for (auto& rowkv : orow->children) {
            if ((rowkv.second->type & DT_ARR_OR_OBJ) != 0) {
                superKeylist.push_back(rowkv.first);
                FlattenSingleRow_FULL_RECURSIVE(rowkv.second, keySep, restOfRow, superKeylist);
                superKeylist.pop_back();
            } else {
                restOfRow.set(keyformat(rowkv.first), rowkv.second);
            }
        }
    } else {
        restOfRow.set(superKey, row);
    }
}

void JsonTabularizer::BuildTableHelper_FULL_RECURSIVE(const JNodePtr& obj,
                                                      std::vector<JNodePtr>& result,
                                                      const std::string& keySep) {
    for (JNodePtr& child : AsArr(obj)->children) {
        auto row = std::make_shared<JObject>();
        std::vector<std::string> superKeylist;
        FlattenSingleRow_FULL_RECURSIVE(child, keySep, row->children, superKeylist);
        result.push_back(row);
    }
}

void JsonTabularizer::BuildTableHelper_STRINGIFY_ITERABLES(const JNodePtr& obj,
                                                           std::vector<JNodePtr>& result,
                                                           const std::string& keySep) {
    if (JArray* aobj = AsArr(obj)) {
        for (JNodePtr& child : aobj->children) {
            auto row = std::make_shared<JObject>();
            if (JArray* achild = AsArr(child)) {
                for (size_t ii = 0; ii < achild->Length(); ii++) {
                    std::string key = "col" + std::to_string(ii + 1);
                    JNodePtr subchild = achild->children[ii];
                    row->children.set(key, (subchild->type & DT_ARR_OR_OBJ) != 0
                                               ? JNode::Str(subchild->ToString())
                                               : subchild);
                }
            } else if (JObject* ochild = AsObj(child)) {
                for (auto& subkv : ochild->children) {
                    row->children.set(subkv.first, (subkv.second->type & DT_ARR_OR_OBJ) != 0
                                                       ? JNode::Str(subkv.second->ToString())
                                                       : subkv.second);
                }
            }
            result.push_back(row);
        }
        return;
    }
    JObject* oobj = AsObj(obj);
    OrderedMap arrDict;
    OrderedMap notArrDict;
    size_t lenArr = 0;
    for (auto& kv : oobj->children) {
        if (JArray* varr = AsArr(kv.second)) {
            arrDict.set(kv.first, kv.second);
            if (varr->Length() > lenArr) lenArr = varr->Length();
        } else {
            notArrDict.set(kv.first, kv.second);
        }
    }
    notArrDict = ResolveHang(notArrDict, keySep);
    for (size_t ii = 0; ii < lenArr; ii++) {
        auto newrec = std::make_shared<JObject>();
        for (auto& kv : arrDict) {
            JArray* v = AsArr(kv.second);
            if (ii >= v->Length()) {
                newrec->children.set(kv.first, JNode::Str(""));
            } else {
                JNodePtr subchild = v->children[ii];
                newrec->children.set(kv.first, (subchild->type & DT_ARR_OR_OBJ) != 0
                                                   ? JNode::Str(subchild->ToString())
                                                   : subchild);
            }
        }
        for (auto& kv : notArrDict) newrec->children.set(kv.first, kv.second);
        result.push_back(newrec);
    }
}

std::shared_ptr<JArray> JsonTabularizer::BuildTable(const JNodePtr& obj, const JNodePtr& schema,
                                                    const std::string& keySep) {
    std::vector<JNodePtr> result;
    std::vector<std::string> superKeylist;
    OrderedMap emptyRow;
    if (strategy == JsonTabularizerStrategy::NO_RECURSION) {
        JsonFormat cls = ClassifySchema(schema);
        if ((cls & JF_ANY_TABLE) == 0)
            throw JsonToolsError("This JSON does not have a tabular format, and BuildTable doesn't know how to proceed.");
        auto emptyPath = std::make_shared<JArray>();
        BuildTableHelper_DEFAULT(obj, cls, 0, emptyPath.get(), result, emptyRow, superKeylist, keySep);
    } else if (strategy == JsonTabularizerStrategy::FULL_RECURSIVE) {
        if (!AsArr(obj))
            throw JsonToolsError("Full recursive strategy does not work on non-array JSON.");
        BuildTableHelper_FULL_RECURSIVE(obj, result, keySep);
    } else if (strategy == JsonTabularizerStrategy::STRINGIFY_ITERABLES) {
        BuildTableHelper_STRINGIFY_ITERABLES(obj, result, keySep);
    } else {
        auto tabPaths = FindTabsInSchema(schema);
        if (tabPaths.empty()) return std::make_shared<JArray>();
        if (tabPaths.size() > 1)
            throw JsonToolsError("This JSON contains " + std::to_string(tabPaths.size()) +
                                 " possible tables, and BuildTable doesn't know how to proceed.");
        JArray* path = AsArr(tabPaths[0].first);
        JsonFormat cls = tabPaths[0].second;
        // the C# stores path components as JNodes already; array levels are
        // Int(0) placeholders and object levels are strings
        // convert Int placeholders: BuildTableHelper_DEFAULT branches on the
        // JSON type at each level, so the placeholder value never matters
        BuildTableHelper_DEFAULT(obj, cls, 0, path, result, emptyRow, superKeylist, keySep);
    }
    auto out = std::make_shared<JArray>();
    out->children = std::move(result);
    return out;
}

void JsonTabularizer::ApplyQuotesIfNeeded(std::string& sb, const std::string& s, char delim,
                                          char quote) {
    bool needQuotes = false;
    for (char c : s) {
        if (c == delim || c == '\r' || c == '\n' || c == quote) {
            needQuotes = true;
            break;
        }
    }
    if (needQuotes) {
        sb += quote;
        for (char c : s) {
            sb += c;
            if (c == quote) sb += quote;
        }
        sb += quote;
    } else {
        sb += s;
    }
}

void JsonTabularizer::CsvStringToSb(std::string& sb, const JNodePtr& jnode, char delim, char quote,
                                    bool boolsAsInts) {
    std::string val;
    switch (jnode->type) {
        case DT_STR:
            val = jnode->GetStr();
            break;
        case DT_NULL:
            return; // nulls are empty entries
        case DT_BOOL:
            sb += jnode->GetBool() ? (boolsAsInts ? "1" : "true") : (boolsAsInts ? "0" : "false");
            return;
        default:
            val = jnode->ToString();
            break;
    }
    ApplyQuotesIfNeeded(sb, val, delim, quote);
}

std::string JsonTabularizer::TableToCsv(JArray* table, char delim, char quote,
                                        const std::string& newline,
                                        const std::vector<std::string>* headerIn,
                                        bool boolsAsInts) {
    std::vector<std::string> header;
    if (headerIn) {
        header = *headerIn;
    } else {
        std::unordered_set<std::string> seen;
        for (JNodePtr& child : table->children) {
            for (auto& kv : AsObj(child)->children) {
                if (seen.insert(kv.first).second) header.push_back(kv.first);
            }
        }
        std::sort(header.begin(), header.end());
    }
    std::string sb;
    for (size_t ii = 0; ii < header.size(); ii++) {
        ApplyQuotesIfNeeded(sb, header[ii], delim, quote);
        if (ii < header.size() - 1) sb += delim;
    }
    sb += newline;
    for (JNodePtr& row : table->children) {
        JObject* orow = AsObj(row);
        for (size_t ii = 0; ii < header.size(); ii++) {
            JNodePtr jnode;
            if (!orow->TryGetValue(header[ii], jnode)) {
                if (ii < header.size() - 1) sb += delim;
                continue;
            }
            CsvStringToSb(sb, jnode, delim, quote, boolsAsInts);
            if (ii < header.size() - 1) sb += delim;
        }
        sb += newline;
    }
    return sb;
}

} // namespace jsontools
