// JsonSchemaMaker.cpp — see JsonSchemaMaker.h for the port notes.

#include "JsonSchemaMaker.h"

#include <algorithm>
#include <unordered_set>

namespace jsontools {

std::string JsonSchemaMaker::TypeName(Dtype x) {
    switch (x) {
        case DT_STR: return "string";
        case DT_FLOAT: return "number";
        case DT_INT: return "integer";
        case DT_ARR: return "array";
        case DT_OBJ: return "object";
        case DT_BOOL: return "boolean";
        case DT_NULL: return "null";
        default: {
            std::string s = FormatDtype(x);
            for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            return s;
        }
    }
}

const std::unordered_map<std::string, Dtype>& JsonSchemaMaker::TypeNameToDtype() {
    static auto* m = new std::unordered_map<std::string, Dtype>{
        {"string", DT_STR}, {"number", DT_FLOAT}, {"integer", DT_INT}, {"array", DT_ARR},
        {"object", DT_OBJ}, {"boolean", DT_BOOL}, {"null", DT_NULL},
    };
    return *m;
}

bool JsonSchemaMaker::IsSingleDtype(Dtype x) {
    return x == DT_ARR || x == DT_STR || x == DT_FLOAT || x == DT_INT || x == DT_OBJ ||
           x == DT_BOOL || x == DT_NULL || x == DT_REGEX || x == DT_SLICE;
}

Dtype JsonSchemaMaker::DtypeUnion(Dtype a, Dtype b) {
    uint16_t together = static_cast<uint16_t>(a) | static_cast<uint16_t>(b);
    if ((together & DT_FLOAT_OR_INT) == DT_FLOAT_OR_INT) {
        // in JSON Schema, "number" is a supertype of "integer"
        together ^= DT_INT;
    }
    return static_cast<Dtype>(together);
}

// internal-form helpers
static Dtype SchemaType(const JNodePtr& schema) {
    JObject* obj = AsObj(schema);
    JNodePtr typeNode;
    if (obj && obj->TryGetValue("type", typeNode) && typeNode->type == DT_INT)
        return static_cast<Dtype>(typeNode->GetInt());
    return DT_TYPELESS;
}
static bool SchemaHas(const JNodePtr& schema, const char* key) {
    JObject* obj = AsObj(schema);
    return obj && obj->children.contains(key);
}
static JNodePtr SchemaGet(const JNodePtr& schema, const char* key) {
    return AsObj(schema)->at(key);
}
static JNodePtr MakeTypeSchema(Dtype t) {
    auto s = std::make_shared<JObject>();
    s->children.set("type", JNode::Int(static_cast<int64_t>(t)));
    return s;
}

static JNodePtr CompactAnyOf(const std::vector<JNodePtr>& anyOf);

JNodePtr JsonSchemaMaker::MergeSchemas(JNodePtr s1, JNodePtr s2) {
    JObject* s1obj = AsObj(s1);
    JObject* s2obj = AsObj(s2);
    if (!s1obj || s1obj->Length() == 0) return s2;
    if (!s2obj || s2obj->Length() == 0) return s1;
    std::vector<JNodePtr> anyof;
    Dtype scaltypes = DT_TYPELESS;
    bool s1_anyOf = !SchemaHas(s1, "type");
    bool s2_anyOf = !SchemaHas(s2, "type");
    Dtype s1type = s1_anyOf ? DT_TYPELESS : SchemaType(s1);
    Dtype s2type = s2_anyOf ? DT_TYPELESS : SchemaType(s2);
    bool s1_typeless = s1type == DT_TYPELESS;
    bool s2_typeless = s2type == DT_TYPELESS;
    auto accumulateAnyof = [&](const JNodePtr& s) {
        for (JNodePtr& v : AsArr(SchemaGet(s, "anyOf"))->children) {
            Dtype vDtype = SchemaType(v);
            if (vDtype != DT_TYPELESS && (vDtype & DT_SCALAR) != 0)
                scaltypes = DtypeUnion(scaltypes, vDtype);
            else
                anyof.push_back(v);
        }
    };
    if (s1_anyOf) {
        accumulateAnyof(s1);
        if (s2_anyOf) {
            accumulateAnyof(s2);
        } else if ((s2type & DT_ARR_OR_OBJ) == 0) {
            scaltypes = DtypeUnion(scaltypes, s2type);
        } else {
            anyof.push_back(s2);
        }
        if (scaltypes != DT_TYPELESS) anyof.push_back(MakeTypeSchema(scaltypes));
        auto out = std::make_shared<JObject>();
        out->children.set("anyOf", CompactAnyOf(anyof));
        return out;
    }
    if (s2_anyOf) {
        accumulateAnyof(s2);
        if ((s1type & DT_ARR_OR_OBJ) == 0) {
            scaltypes = DtypeUnion(scaltypes, s1type);
        } else {
            anyof.push_back(s1);
        }
        if (scaltypes != DT_TYPELESS) anyof.push_back(MakeTypeSchema(scaltypes));
        auto out = std::make_shared<JObject>();
        out->children.set("anyOf", CompactAnyOf(anyof));
        return out;
    }
    if ((s1type & DT_ARR_OR_OBJ) == 0) {
        if (s1_typeless) return s2;
        if ((s2type & DT_ARR_OR_OBJ) == 0) return MakeTypeSchema(DtypeUnion(s1type, s2type));
        anyof.push_back(MakeTypeSchema(s1type));
        anyof.push_back(s2);
        auto out = std::make_shared<JObject>();
        auto anyofArr = std::make_shared<JArray>();
        anyofArr->children = anyof;
        out->children.set("anyOf", anyofArr);
        return out;
    }
    if ((s2type & DT_ARR_OR_OBJ) == 0) {
        if (s2_typeless) return s1;
        anyof.push_back(MakeTypeSchema(s2type));
        anyof.push_back(s1);
        auto out = std::make_shared<JObject>();
        auto anyofArr = std::make_shared<JArray>();
        anyofArr->children = anyof;
        out->children.set("anyOf", anyofArr);
        return out;
    }
    if (s1type == DT_ARR && s2type == DT_ARR) {
        bool s1_hasItems = SchemaHas(s1, "items");
        bool s2_hasItems = SchemaHas(s2, "items");
        JNodePtr combinedItems;
        if (!s1_hasItems) {
            if (!s2_hasItems) return MakeTypeSchema(DT_ARR);
            combinedItems = SchemaGet(s2, "items");
        } else if (!s2_hasItems) {
            combinedItems = SchemaGet(s1, "items");
        } else {
            combinedItems = MergeSchemas(SchemaGet(s1, "items"), SchemaGet(s2, "items"));
        }
        auto out = MakeTypeSchema(DT_ARR);
        AsObj(out)->children.set("items", combinedItems);
        return out;
    }
    if (s1type == DT_OBJ && s2type == DT_OBJ) {
        bool s1_hasProps = SchemaHas(s1, "properties");
        bool s2_hasProps = SchemaHas(s2, "properties");
        if (!s1_hasProps) {
            if (!s2_hasProps) return MakeTypeSchema(DT_OBJ);
            auto out = MakeTypeSchema(DT_OBJ);
            AsObj(out)->children.set("properties", SchemaGet(s2, "properties"));
            AsObj(out)->children.set("required", std::make_shared<JArray>());
            return out;
        }
        if (!s2_hasProps) {
            auto out = MakeTypeSchema(DT_OBJ);
            AsObj(out)->children.set("properties", SchemaGet(s1, "properties"));
            AsObj(out)->children.set("required", std::make_shared<JArray>());
            return out;
        }
        auto props = std::make_shared<JObject>();
        auto out = MakeTypeSchema(DT_OBJ);
        AsObj(out)->children.set("properties", props);
        JObject* s1props = AsObj(SchemaGet(s1, "properties"));
        JObject* s2props = AsObj(SchemaGet(s2, "properties"));
        std::unordered_set<std::string> s1_keyset;
        std::unordered_set<std::string> s2_keyset;
        for (auto& kv : s1props->children) s1_keyset.insert(kv.first);
        for (auto& kv : s2props->children) s2_keyset.insert(kv.first);
        std::unordered_set<std::string> sharedKeys;
        for (const std::string& k : s1_keyset)
            if (s2_keyset.count(k)) sharedKeys.insert(k);
        for (const std::string& k : sharedKeys) {
            s1_keyset.erase(k);
            s2_keyset.erase(k);
        }
        auto getRequired = [](const JNodePtr& s,
                              const std::unordered_set<std::string>& fallback) {
            std::unordered_set<std::string> out;
            if (SchemaHas(s, "required")) {
                for (JNodePtr& v : AsArr(SchemaGet(s, "required"))->children) out.insert(v->GetStr());
                return out;
            }
            return fallback;
        };
        std::unordered_set<std::string> s1_req = getRequired(s1, s1_keyset);
        std::unordered_set<std::string> s2_req = getRequired(s2, s2_keyset);
        auto required = std::make_shared<JArray>();
        for (const std::string& k : s1_req)
            if (s2_req.count(k)) required->children.push_back(JNode::Str(k));
        AsObj(out)->children.set("required", required);
        for (const std::string& k : sharedKeys)
            props->children.set(k, MergeSchemas(s1props->at(k), s2props->at(k)));
        for (auto& kv : s1props->children)
            if (s1_keyset.count(kv.first)) props->children.set(kv.first, kv.second);
        for (auto& kv : s2props->children)
            if (s2_keyset.count(kv.first)) props->children.set(kv.first, kv.second);
        return out;
    }
    // one is an array and the other is an object
    anyof.push_back(s1);
    anyof.push_back(s2);
    auto out = std::make_shared<JObject>();
    auto anyofArr = std::make_shared<JArray>();
    anyofArr->children = anyof;
    out->children.set("anyOf", anyofArr);
    return out;
}

static JNodePtr CompactAnyOf(const std::vector<JNodePtr>& anyOf) {
    JNodePtr objectAnyof = std::make_shared<JObject>();
    JNodePtr arrayAnyof = std::make_shared<JObject>();
    Dtype scaltypes = DT_TYPELESS;
    for (const JNodePtr& subschema : anyOf) {
        Dtype subtipe = SchemaType(subschema);
        if (subtipe == DT_TYPELESS && !SchemaHas(subschema, "type")) continue;
        if ((subtipe & DT_SCALAR) != 0)
            scaltypes = JsonSchemaMaker::DtypeUnion(scaltypes, subtipe);
        else if (subtipe == DT_ARR)
            arrayAnyof = JsonSchemaMaker::MergeSchemas(arrayAnyof, subschema);
        else if (subtipe == DT_OBJ)
            objectAnyof = JsonSchemaMaker::MergeSchemas(objectAnyof, subschema);
    }
    auto result = std::make_shared<JArray>();
    if (scaltypes != DT_TYPELESS) result->children.push_back(MakeTypeSchema(scaltypes));
    if (AsObj(arrayAnyof) && AsObj(arrayAnyof)->Length() > 0) result->children.push_back(arrayAnyof);
    if (AsObj(objectAnyof) && AsObj(objectAnyof)->Length() > 0) result->children.push_back(objectAnyof);
    return result;
}

JNodePtr JsonSchemaMaker::BuildSchema(const JNodePtr& json) {
    Dtype tipe = json->type;
    JNodePtr schema = MakeTypeSchema(tipe);
    if (tipe == DT_ARR) {
        JNodePtr items = std::make_shared<JObject>();
        Dtype scalarTypes = DT_TYPELESS;
        for (JNodePtr& elt : AsArr(json)->children) {
            if ((elt->type & DT_SCALAR) != 0)
                scalarTypes = DtypeUnion(scalarTypes, elt->type);
            else
                items = MergeSchemas(items, BuildSchema(elt));
        }
        if (scalarTypes != 0) items = MergeSchemas(MakeTypeSchema(scalarTypes), items);
        if (AsObj(items)->Length() > 0) AsObj(schema)->children.set("items", items);
    } else if (JObject* obj = AsObj(json)) {
        auto props = std::make_shared<JObject>();
        for (auto& kv : obj->children) props->children.set(kv.first, BuildSchema(kv.second));
        if (props->Length() > 0) {
            auto required = std::make_shared<JArray>();
            for (auto& kv : obj->children) required->children.push_back(JNode::Str(kv.first));
            AsObj(schema)->children.set("required", required);
            AsObj(schema)->children.set("properties", props);
        }
    }
    return schema;
}

// ordering for anyOf lists: scalars (alphabetical) < arrays < objects
static int SchemaComparer(const JNodePtr& x, const JNodePtr& y) {
    JObject* xobj = AsObj(x);
    JObject* yobj = AsObj(y);
    JNodePtr xtype, ytype;
    if (!xobj || !yobj || !xobj->TryGetValue("type", xtype) || !yobj->TryGetValue("type", ytype))
        return 0;
    if (xtype->type == DT_ARR) {
        if (ytype->type == DT_ARR) return 0;
        return -1;
    }
    if (ytype->type == DT_ARR) return 1;
    std::string xtypeStr = xtype->GetStr();
    std::string ytypeStr = ytype->GetStr();
    if (xtypeStr == "array") {
        if (ytypeStr == "array") return 0;
        if (ytypeStr == "object") return -1;
        return 1;
    }
    if (xtypeStr == "object") return ytypeStr == "object" ? 0 : 1;
    if (ytypeStr == "object") return -1;
    return x->ToString().compare(y->ToString());
}

JNodePtr JsonSchemaMaker::SchemaToJNode(const JNodePtr& schema) {
    JObject* obj = AsObj(schema);
    if (obj) {
        auto kids = std::make_shared<JObject>();
        for (auto& kv : obj->children) {
            if (kv.first == "type" && kv.second->type == DT_INT) {
                Dtype type = static_cast<Dtype>(kv.second->GetInt());
                if (IsSingleDtype(type)) {
                    kids->children.set("type", JNode::Str(TypeName(type)));
                } else {
                    auto typelist = std::make_shared<JArray>();
                    if ((type & DT_BOOL) != 0) typelist->children.push_back(JNode::Str("boolean"));
                    if ((type & DT_INT) != 0) typelist->children.push_back(JNode::Str("integer"));
                    if ((type & DT_NULL) != 0) typelist->children.push_back(JNode::Str("null"));
                    if ((type & DT_FLOAT) != 0) typelist->children.push_back(JNode::Str("number"));
                    if ((type & DT_STR) != 0) typelist->children.push_back(JNode::Str("string"));
                    kids->children.set("type", typelist);
                }
            } else if (kv.first == "anyOf") {
                auto children = std::make_shared<JArray>();
                for (JNodePtr& v : AsArr(kv.second)->children)
                    children->children.push_back(SchemaToJNode(v));
                std::stable_sort(children->children.begin(), children->children.end(),
                                 [](const JNodePtr& a, const JNodePtr& b) {
                                     return SchemaComparer(a, b) < 0;
                                 });
                kids->children.set("anyOf", children);
            } else if (kv.first == "required") {
                auto children = std::make_shared<JArray>();
                for (JNodePtr& v : AsArr(kv.second)->children) children->children.push_back(v);
                std::stable_sort(children->children.begin(), children->children.end(),
                                 [](const JNodePtr& a, const JNodePtr& b) {
                                     return a->GetStr() < b->GetStr();
                                 });
                kids->children.set("required", children);
            } else if (kv.second->type == DT_OBJ) {
                kids->children.set(kv.first, SchemaToJNode(kv.second));
            } else {
                kids->children.set(kv.first, kv.second);
            }
        }
        return kids;
    }
    return schema;
}

JNodePtr JsonSchemaMaker::GetSchema(const JNodePtr& json) {
    JNodePtr internal = BuildSchema(json);
    auto out = std::make_shared<JObject>();
    out->children.set("$schema", JNode::Str("https://json-schema.org/draft/2020-12/schema"));
    for (auto& kv : AsObj(internal)->children) out->children.set(kv.first, kv.second);
    return SchemaToJNode(out);
}

} // namespace jsontools
