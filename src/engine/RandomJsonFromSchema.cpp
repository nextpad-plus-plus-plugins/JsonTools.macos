// RandomJsonFromSchema.cpp — see RandomJsonFromSchema.h.

#include "RandomJsonFromSchema.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_set>

#include "RemesPath.h" // RandomNext / RandomNextDouble

namespace jsontools {

static const char* PRINTABLE =
    "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~ \t\r\n";

JNodePtr RandomJsonFromSchema::RandomBoolean() { return JNode::Bool(RandomNext(2) == 1); }

JNodePtr RandomJsonFromSchema::RandomFloat() {
    return JNode::Float(RandomNextDouble() * 10 - 5);
}

JNodePtr RandomJsonFromSchema::RandomIntNode() {
    return JNode::Int(static_cast<int64_t>(RandomNext(2000001)) - 1000000);
}

static JNodePtr RandomNumberBetweenMinAndMax(double min, double max) {
    if (min == -std::numeric_limits<double>::infinity()) min = -1e6;
    if (max == std::numeric_limits<double>::infinity()) max = 1e6;
    if (min > max) min = max - 1e6;
    double randNumber = RandomNextDouble() * (max - min) + min;
    if (RandomNextDouble() < 0.5) return JNode::Float(randNumber);
    return JNode::Int(static_cast<int64_t>(std::llround(randNumber)));
}

static JNodePtr RandomIntegerBetweenMinAndMax(double min, double max) {
    JNodePtr rand = RandomNumberBetweenMinAndMax(min, max);
    if (rand->type == DT_FLOAT) return JNode::Int(static_cast<int64_t>(std::llround(rand->GetDouble())));
    return rand;
}

JNodePtr RandomJsonFromSchema::RandomNumber() {
    if (RandomNextDouble() < 0.5) return RandomIntNode();
    return RandomFloat();
}

JNodePtr RandomJsonFromSchema::RandomString(const JNodePtr& schema) {
    int64_t minLength = 0;
    int64_t exclusiveMaxLength = 11;
    if (JObject* obj = AsObj(schema)) {
        // "pattern" would be used here if the random-string-from-regex
        // generator were ported; patterns are silently ignored instead
        JNodePtr minLengthNode, maxLengthNode;
        if (obj->TryGetValue("minLength", minLengthNode) && minLengthNode->type == DT_INT)
            minLength = minLengthNode->GetInt();
        if (obj->TryGetValue("maxLength", maxLengthNode) && maxLengthNode->type == DT_INT)
            exclusiveMaxLength = maxLengthNode->GetInt() + 1;
    }
    int64_t length = exclusiveMaxLength > minLength
        ? minLength + static_cast<int64_t>(RandomNext(static_cast<uint64_t>(exclusiveMaxLength - minLength)))
        : minLength;
    std::string out;
    for (int64_t ii = 0; ii < length; ii++) {
        if (extendedAsciiStrings_) {
            // code points 1-255 (0 excluded), encoded as UTF-8
            uint32_t cp = 1 + static_cast<uint32_t>(RandomNext(255));
            AppendCodePoint(out, cp);
        } else {
            out += PRINTABLE[RandomNext(strlen(PRINTABLE))];
        }
    }
    return JNode::Str(std::move(out));
}

JNodePtr RandomJsonFromSchema::RandomArray(const JNodePtr& schema, JObject* refs,
                                           int recursionDepth) {
    JObject* children = AsObj(schema);
    JNodePtr items;
    if (!children || !children->TryGetValue("items", items)) return std::make_shared<JArray>();
    int64_t minlen = minArrayLength_;
    JNodePtr minItemsNode, maxItemsNode;
    if (children->TryGetValue("minItems", minItemsNode) && minItemsNode->type == DT_INT)
        minlen = minItemsNode->GetInt();
    int64_t maxlen = maxArrayLength_;
    if (children->TryGetValue("maxItems", maxItemsNode) && maxItemsNode->type == DT_INT)
        maxlen = maxItemsNode->GetInt();
    int64_t length = maxlen >= minlen
        ? minlen + static_cast<int64_t>(RandomNext(static_cast<uint64_t>(maxlen - minlen + 1)))
        : minlen;
    auto out = std::make_shared<JArray>();
    JNodePtr contains;
    bool hasContains = children->TryGetValue("contains", contains);
    if (hasContains) {
        int64_t minContains = 1;
        JNodePtr minContainsNode, maxContainsNode;
        if (children->TryGetValue("minContains", minContainsNode) && minContainsNode->type == DT_INT)
            minContains = minContainsNode->GetInt();
        int64_t maxContains = length;
        if (children->TryGetValue("maxContains", maxContainsNode) && maxContainsNode->type == DT_INT)
            maxContains = maxContainsNode->GetInt();
        int64_t ncontains = maxContains >= minContains
            ? minContains + static_cast<int64_t>(RandomNext(static_cast<uint64_t>(maxContains - minContains + 1)))
            : minContains;
        length -= ncontains;
        for (int64_t ii = 0; ii < ncontains; ii++)
            out->children.push_back(Helper(contains, refs, recursionDepth + 1));
    }
    for (int64_t ii = 0; ii < length; ii++)
        out->children.push_back(Helper(items, refs, recursionDepth + 1));
    if (hasContains) {
        for (size_t ii = 0; ii < out->children.size(); ii++) {
            size_t jj = static_cast<size_t>(RandomNext(ii + 1));
            std::swap(out->children[ii], out->children[jj]);
        }
    }
    return out;
}

JNodePtr RandomJsonFromSchema::RandomObject(const JNodePtr& schema, JObject* refs,
                                            int recursionDepth) {
    JObject* children = AsObj(schema);
    auto result = std::make_shared<JObject>();
    // (patternProperties would be used here with the regex generator)
    JNodePtr properties;
    if (!children->TryGetValue("properties", properties)) return result;
    JObject* propertiesObj = AsObj(properties);
    std::vector<std::string> optionalKeys;
    std::vector<std::string> requiredKeys;
    std::unordered_set<std::string> requiredSet;
    JNodePtr requiredNode;
    if (children->TryGetValue("required", requiredNode)) {
        if (JArray* requiredArr = AsArr(requiredNode)) {
            for (JNodePtr& req : requiredArr->children) {
                requiredKeys.push_back(req->GetStr());
                requiredSet.insert(req->GetStr());
            }
        }
    }
    for (auto& kv : propertiesObj->children)
        if (!requiredSet.count(kv.first)) optionalKeys.push_back(kv.first);
    std::vector<std::string> optionalIncluded;
    if (!optionalKeys.empty()) {
        size_t numOptionalIncluded = RandomNext(optionalKeys.size() + 1);
        std::unordered_set<size_t> indicesChosen;
        for (size_t ii = 0; ii < numOptionalIncluded; ii++) {
            size_t idx = RandomNext(optionalKeys.size());
            while (indicesChosen.count(idx)) idx = RandomNext(optionalKeys.size());
            indicesChosen.insert(idx);
            optionalIncluded.push_back(optionalKeys[idx]);
        }
    }
    for (const std::string& k : requiredKeys) {
        JNodePtr subschema;
        if (propertiesObj->TryGetValue(k, subschema))
            result->children.set(k, Helper(subschema, refs, recursionDepth + 1));
    }
    for (const std::string& k : optionalIncluded) {
        result->children.set(k, Helper(propertiesObj->at(k), refs, recursionDepth + 1));
    }
    return result;
}

JNodePtr RandomJsonFromSchema::RandomAnything(int recursionDepth) {
    auto emptySchema = std::make_shared<JObject>();
    switch (RandomNext(7)) {
        case 0: return RandomBoolean();
        case 1: return RandomFloat();
        case 2: return RandomIntNode();
        case 3: return std::make_shared<JNode>();
        case 4: return RandomString(emptySchema);
        case 5: return RandomArray(emptySchema, emptySchema.get(), recursionDepth);
        default: return RandomObject(emptySchema, emptySchema.get(), recursionDepth);
    }
}

JNodePtr RandomJsonFromSchema::Dispatch(const std::string& type, const JNodePtr& schema,
                                        JObject* refs, int recursionDepth) {
    if (type == "array") return RandomArray(schema, refs, recursionDepth);
    if (type == "boolean") return RandomBoolean();
    if (type == "integer") return RandomIntNode();
    if (type == "null") return std::make_shared<JNode>();
    if (type == "number") return RandomNumber();
    if (type == "object") return RandomObject(schema, refs, recursionDepth);
    if (type == "string") return RandomString(schema);
    throw SchemaValidationException("Unknown schema type \"" + type + "\"");
}

JNodePtr RandomJsonFromSchema::Helper(const JNodePtr& schema, JObject* refs, int recursionDepth) {
    if (recursionDepth >= RECURSION_LIMIT)
        throw SchemaValidationException(
            "Validation failed at location root because the recursion limit for validation was reached");
    JObject* obj = AsObj(schema);
    if (!obj) {
        if (!schema)
            throw SchemaValidationException("Schema was null (in the programmatic sense, not a JSON element representing null)");
        if (schema->type != DT_BOOL)
            throw SchemaValidationException("A JSON schema must be an object or a boolean");
        if (schema->GetBool()) return RandomAnything(recursionDepth);
        return std::make_shared<JNode>();
    }
    if (obj->Length() == 0) return RandomAnything(recursionDepth);
    JNodePtr enum_;
    if (obj->TryGetValue("enum", enum_)) {
        auto& enumMembers = AsArr(enum_)->children;
        return enumMembers[RandomNext(enumMembers.size())]->Copy();
    }
    JNodePtr typeNode;
    if (!obj->TryGetValue("type", typeNode)) {
        JNodePtr anyOf;
        if (!obj->TryGetValue("anyOf", anyOf)) {
            JNodePtr refnode;
            if (!obj->TryGetValue("$ref", refnode))
                throw SchemaValidationException("A schema must have at least one of the \"type\", \"enum\", \"anyOf\" keywords.");
            std::string refname = refnode->GetStr();
            size_t slash = refname.rfind('/');
            if (slash != std::string::npos) refname = refname.substr(slash + 1);
            JNodePtr reference;
            if (!refs->TryGetValue(refname, reference))
                throw SchemaValidationException("Reference " + refname + " to an undefined schema");
            return Helper(reference, refs, recursionDepth);
        }
        auto& anyOfArr = AsArr(anyOf)->children;
        return Helper(anyOfArr[RandomNext(anyOfArr.size())], refs, recursionDepth);
    }
    if (JArray* typeArr = AsArr(typeNode)) {
        JNodePtr typeChoice = typeArr->children[RandomNext(typeArr->Length())];
        return Dispatch(typeChoice->GetStr(), schema, refs, recursionDepth);
    }
    std::string type = typeNode->GetStr();
    bool typeInt = !type.empty() && type[0] == 'i';
    if (typeInt || type == "number") {
        JNodePtr minNode, maxNode;
        double min = obj->TryGetValue("minimum", minNode) ? minNode->ToDouble()
                                                          : -std::numeric_limits<double>::infinity();
        double max = obj->TryGetValue("maximum", maxNode) ? maxNode->ToDouble()
                                                          : std::numeric_limits<double>::infinity();
        if (!std::isinf(min) || !std::isinf(max)) {
            if (typeInt) return RandomIntegerBetweenMinAndMax(min, max);
            return RandomNumberBetweenMinAndMax(min, max);
        }
    }
    return Dispatch(type, schema, refs, recursionDepth);
}

JNodePtr RandomJsonFromSchema::RandomJson(const JNodePtr& schema, int64_t minArrayLength,
                                          int64_t maxArrayLength, bool extendedAsciiStrings,
                                          bool usePatterns) {
    JObject* schemobj = AsObj(schema);
    JNodePtr refs;
    if (!schemobj ||
        (!schemobj->TryGetValue("$defs", refs) && !schemobj->TryGetValue("definitions", refs)))
        refs = std::make_shared<JObject>();
    RandomJsonFromSchema rjfs(minArrayLength, maxArrayLength, extendedAsciiStrings, usePatterns);
    return rjfs.Helper(schema, AsObj(refs), 0);
}

} // namespace jsontools
