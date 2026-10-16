// JsonSchemaValidator.cpp — see JsonSchemaValidator.h.

#include "JsonSchemaValidator.h"

#include <cmath>
#include <limits>

#include "JsonSchemaMaker.h"
#include "RandomJsonFromSchema.h" // SchemaValidationException
#include "RemesPath.h"            // Regex helpers already in JNode.h; LruCache unused here

namespace jsontools {

// C# double.ToString(): integral doubles print without ".0"
static std::string CSharpNum(double d) {
    std::string s = JNode::DoubleToString(d);
    if (s.size() > 2 && s.compare(s.size() - 2, 2, ".0") == 0) s.resize(s.size() - 2);
    return s;
}

static JsonLint MakeSchemaLint(JsonLintType /*problemType*/, const std::string& msg, int64_t position) {
    // the C# stores all schema problems as SCHEMA_TYPE_MISMATCH with a prebuilt message
    return JsonLint(JsonLintType::SCHEMA_TYPE_MISMATCH, position, '\x00', msg);
}

std::string JsonSchemaValidator::LintsAsJArrayString(const std::vector<JsonLint>& lints) {
    auto arr = std::make_shared<JArray>();
    for (const JsonLint& l : lints) arr->children.push_back(l.ToJson());
    return arr->ToString();
}

JsonSchemaValidator::ValidationFunc JsonSchemaValidator::CompileValidationFunc(
    const JNodePtr& schema, int64_t maxLintCount) {
    std::shared_ptr<JObject> defs = std::make_shared<JObject>();
    if (JObject* obj = AsObj(schema)) {
        JNodePtr defsNode;
        if (obj->TryGetValue("definitions", defsNode) || obj->TryGetValue("$defs", defsNode)) {
            if (JObject* d = AsObj(defsNode)) {
                for (auto& kv : d->children) defs->children.set(kv.first, kv.second);
            }
        }
    }
    ValidationHelperFunc helper = CompileValidationHelperFunc(schema, defs, 0, maxLintCount);
    return [helper](const JNodePtr& x, std::vector<JsonLint>& lints) {
        lints.clear();
        helper(x, lints);
        return lints.empty();
    };
}

bool JsonSchemaValidator::Validates(const JNodePtr& json, const JNodePtr& schema,
                                    int64_t maxLintCount, std::vector<JsonLint>& lints) {
    ValidationFunc validator = CompileValidationFunc(schema, maxLintCount);
    return validator(json, lints);
}

JsonSchemaValidator::ValidationHelperFunc JsonSchemaValidator::CompileValidationHelperFunc(
    const JNodePtr& schema_, const std::shared_ptr<JObject>& definitions, int recursions,
    int64_t maxLintCount) {
    if (recursions == RECURSION_LIMIT)
        return [](const JNodePtr&, std::vector<JsonLint>& lints) {
            lints.push_back(MakeSchemaLint(JsonLintType::SCHEMA_RECURSION_LIMIT_REACHED,
                                           "validation has a maximum depth of 64", 0));
        };
    JObject* schema = AsObj(schema_);
    if (!schema) {
        if (!schema_)
            throw SchemaValidationException("Schema was null (in the programmatic sense, not a JSON element representing null)");
        if (schema_->type == DT_BOOL) {
            if (schema_->GetBool()) return [](const JNodePtr&, std::vector<JsonLint>&) {};
            return [](const JNodePtr& x, std::vector<JsonLint>& lints) {
                lints.push_back(MakeSchemaLint(JsonLintType::SCHEMA_FALSE_SCHEMA,
                                               "the schema is false, so nothing will validate.",
                                               x->position));
            };
        }
        throw SchemaValidationException("JSON schema must be object or boolean, got type " +
                                        FormatDtype(schema_->type));
    }
    if (schema->Length() == 0) return [](const JNodePtr&, std::vector<JsonLint>&) {};
    JNodePtr refnode;
    if (schema->TryGetValue("$ref", refnode)) {
        std::string reference = refnode->GetStr();
        size_t slash = reference.rfind('/');
        if (slash != std::string::npos) reference = reference.substr(slash + 1);
        JNodePtr def;
        if (!definitions->TryGetValue(reference, def))
            throw SchemaValidationException("\"$ref\" " + refnode->ToString() +
                                            " does not point to a definition defined in the definitions:\r\n" +
                                            definitions->ToString());
        return CompileValidationHelperFunc(def, definitions, recursions, maxLintCount);
    }
    JNodePtr enum_;
    if (schema->TryGetValue("enum", enum_) && AsArr(enum_)) {
        auto enumArr = std::static_pointer_cast<JArray>(enum_);
        return [enumArr](const JNodePtr& json, std::vector<JsonLint>& lints) {
            for (JNodePtr& possible : enumArr->children) {
                if (possible->type != json->type) continue;
                if (possible->Equals(*json)) return;
            }
            lints.push_back(MakeSchemaLint(JsonLintType::SCHEMA_VALUE_NOT_IN_ENUM,
                                           "found value " + json->ToString() +
                                               ", but the only allowed values are " +
                                               enumArr->ToString() + ".",
                                           json->position));
        };
    }
    JNodePtr type;
    if (!schema->TryGetValue("type", type)) {
        JNodePtr anyOf;
        if (!schema->TryGetValue("anyOf", anyOf))
            throw SchemaValidationException("Each schema must have one of the '$ref', 'anyOf', 'type', or 'enum' keywords.");
        auto subValidators = std::make_shared<std::vector<ValidationHelperFunc>>();
        for (JNodePtr& subschema : AsArr(anyOf)->children)
            subValidators->push_back(
                CompileValidationHelperFunc(subschema, definitions, recursions, maxLintCount));
        return [subValidators, anyOf](const JNodePtr& json, std::vector<JsonLint>& lints) {
            size_t initialLintCount = lints.size();
            for (auto& subValidator : *subValidators) {
                size_t lintCountBefore = lints.size();
                subValidator(json, lints);
                if (lints.size() == lintCountBefore) {
                    lints.erase(lints.begin() + static_cast<long>(initialLintCount), lints.end());
                    return;
                }
            }
            lints.push_back(MakeSchemaLint(JsonLintType::SCHEMA_TYPE_ARRAY_MISMATCH,
                                           "found type " + JsonSchemaMaker::TypeName(json->type) +
                                               ", expected one of the types " + anyOf->ToString() + ".",
                                           json->position));
        };
    }
    if (JArray* types = AsArr(type)) {
        Dtype dtypes = DT_TYPELESS;
        for (JNodePtr& typenode : types->children) {
            Dtype dtype_ = JsonSchemaMaker::TypeNameToDtype().at(typenode->GetStr());
            dtypes = JsonSchemaMaker::DtypeUnion(dtypes, dtype_);
        }
        JNodePtr typeNodeCopy = type;
        return [dtypes, typeNodeCopy](const JNodePtr& json, std::vector<JsonLint>& lints) {
            if ((json->type & dtypes) != 0 || (json->type == DT_INT && (dtypes & DT_FLOAT) != 0))
                return;
            lints.push_back(MakeSchemaLint(JsonLintType::SCHEMA_TYPE_ARRAY_MISMATCH,
                                           "found type " + JsonSchemaMaker::TypeName(json->type) +
                                               ", expected one of the types " +
                                               typeNodeCopy->ToString() + ".",
                                           json->position));
        };
    }
    Dtype dtype = JsonSchemaMaker::TypeNameToDtype().at(type->GetStr());
    auto typeMismatchLint = [](const JNodePtr& json, Dtype required) {
        return MakeSchemaLint(JsonLintType::SCHEMA_TYPE_MISMATCH,
                              "found type " + JsonSchemaMaker::TypeName(json->type) +
                                  ", expected type " + JsonSchemaMaker::TypeName(required) + ".",
                              json->position);
    };
    if (dtype == DT_ARR) {
        JNodePtr maxItemsNode, minItemsNode;
        int64_t maxItems = schema->TryGetValue("maxItems", maxItemsNode) ? maxItemsNode->GetInt()
                                                                         : INT64_MAX;
        int64_t minItems = schema->TryGetValue("minItems", minItemsNode) ? minItemsNode->GetInt() : 0;
        auto itemsRightLength = [minItems, maxItems](JArray* arr, std::vector<JsonLint>& lints) {
            int64_t len_ = static_cast<int64_t>(arr->Length());
            if (len_ < minItems) {
                lints.push_back(MakeSchemaLint(
                    JsonLintType::SCHEMA_ARRAY_TOO_SHORT,
                    "array required to have at least " + std::to_string(minItems) +
                        " items, but it has " + std::to_string(len_) + " items.",
                    arr->position));
                return true;
            }
            if (len_ > maxItems) {
                lints.push_back(MakeSchemaLint(
                    JsonLintType::SCHEMA_ARRAY_TOO_LONG,
                    "array required to have no more than " + std::to_string(maxItems) +
                        " items, but it has " + std::to_string(len_) + " items.",
                    arr->position));
                return true;
            }
            return false;
        };
        JNodePtr items;
        if (schema->TryGetValue("items", items)) {
            ValidationHelperFunc itemsValidator =
                CompileValidationHelperFunc(items, definitions, recursions + 1, maxLintCount);
            JNodePtr contains;
            if (schema->TryGetValue("contains", contains)) {
                ValidationHelperFunc containsValidator =
                    CompileValidationHelperFunc(contains, definitions, recursions + 1, maxLintCount);
                JNodePtr minContainsNode, maxContainsNode;
                int64_t minContains = schema->TryGetValue("minContains", minContainsNode)
                    ? minContainsNode->GetInt() : 1;
                int64_t maxContains = schema->TryGetValue("maxContains", maxContainsNode)
                    ? maxContainsNode->GetInt() : INT64_MAX;
                return [itemsRightLength, itemsValidator, containsValidator, minContains,
                        maxContains, contains, maxLintCount,
                        typeMismatchLint](const JNodePtr& json, std::vector<JsonLint>& lints) {
                    JArray* arr = AsArr(json);
                    if (!arr) {
                        lints.push_back(typeMismatchLint(json, DT_ARR));
                        return;
                    }
                    if (itemsRightLength(arr, lints) &&
                        static_cast<int64_t>(lints.size()) > maxLintCount)
                        return;
                    int64_t containsCount = 0;
                    for (JNodePtr& child : arr->children) {
                        size_t lintCountBeforeContains = lints.size();
                        containsValidator(child, lints);
                        size_t lintsCountAfterContains = lints.size();
                        if (lintsCountAfterContains > lintCountBeforeContains) {
                            itemsValidator(child, lints);
                            size_t lintsCountAfterItems = lints.size();
                            if (lintsCountAfterItems == lintsCountAfterContains) {
                                lints.erase(lints.begin() + static_cast<long>(lintCountBeforeContains),
                                            lints.end());
                            } else if (static_cast<int64_t>(lints.size()) > maxLintCount) {
                                return;
                            }
                        } else {
                            containsCount++;
                        }
                    }
                    if (containsCount < minContains || containsCount > maxContains) {
                        std::string msg = maxContains == INT64_MAX
                            ? "Array must have at least " + std::to_string(minContains) +
                                  " items that match \"contains\" schema " + contains->ToString()
                            : "Array must have between " + std::to_string(minContains) + " and " +
                                  std::to_string(maxContains) +
                                  " items that match \"contains\" schema " + contains->ToString();
                        lints.push_back(MakeSchemaLint(JsonLintType::SCHEMA_CONTAINS_VIOLATION, msg,
                                                       arr->position));
                    }
                };
            }
            return [itemsRightLength, itemsValidator, maxLintCount,
                    typeMismatchLint](const JNodePtr& json, std::vector<JsonLint>& lints) {
                JArray* arr = AsArr(json);
                if (!arr) {
                    lints.push_back(typeMismatchLint(json, DT_ARR));
                    return;
                }
                if (itemsRightLength(arr, lints) && static_cast<int64_t>(lints.size()) > maxLintCount)
                    return;
                for (JNodePtr& child : arr->children) {
                    itemsValidator(child, lints);
                    if (static_cast<int64_t>(lints.size()) > maxLintCount) return;
                }
            };
        }
        return [itemsRightLength, typeMismatchLint](const JNodePtr& json,
                                                    std::vector<JsonLint>& lints) {
            if (JArray* arr = AsArr(json)) {
                itemsRightLength(arr, lints);
                return;
            }
            lints.push_back(typeMismatchLint(json, DT_ARR));
        };
    }
    if (dtype == DT_OBJ) {
        auto propsValidators =
            std::make_shared<std::vector<std::pair<std::string, ValidationHelperFunc>>>();
        JNodePtr properties;
        if (schema->TryGetValue("properties", properties)) {
            for (auto& kv : AsObj(properties)->children)
                propsValidators->emplace_back(
                    kv.first,
                    CompileValidationHelperFunc(kv.second, definitions, recursions + 1, maxLintCount));
        }
        auto required = std::make_shared<std::vector<std::string>>();
        JNodePtr requiredNode;
        if (schema->TryGetValue("required", requiredNode)) {
            for (JNodePtr& x : AsArr(requiredNode)->children) required->push_back(x->GetStr());
        }
        using PatternValidator = std::pair<RegexPtr, ValidationHelperFunc>;
        auto patternPropsValidators = std::make_shared<std::vector<PatternValidator>>();
        JNodePtr patternPropsNode;
        if (schema->TryGetValue("patternProperties", patternPropsNode) && AsObj(patternPropsNode)) {
            for (auto& kv : AsObj(patternPropsNode)->children) {
                std::string pat = kv.first;
                size_t pos = 0;
                while ((pos = pat.find("\\\\", pos)) != std::string::npos) {
                    pat.replace(pos, 2, "\\");
                    pos += 1;
                }
                patternPropsValidators->emplace_back(
                    std::make_shared<Regex>(pat, false, false),
                    CompileValidationHelperFunc(kv.second, definitions, recursions + 1, maxLintCount));
            }
        }
        return [propsValidators, required, patternPropsValidators, maxLintCount,
                typeMismatchLint](const JNodePtr& json, std::vector<JsonLint>& lints) {
            JObject* obj = AsObj(json);
            if (!obj) {
                lints.push_back(typeMismatchLint(json, DT_OBJ));
                return;
            }
            for (const std::string& requiredKey : *required) {
                if (!obj->children.contains(requiredKey)) {
                    lints.push_back(MakeSchemaLint(JsonLintType::SCHEMA_OBJECT_MISSING_REQUIRED_KEY,
                                                   "object missing \"required\" key " + requiredKey,
                                                   obj->position));
                    if (static_cast<int64_t>(lints.size()) > maxLintCount) return;
                }
            }
            for (const PatternValidator& pv : *patternPropsValidators) {
                for (auto& kv : obj->children) {
                    if (pv.first->IsMatch(kv.first)) {
                        pv.second(kv.second, lints);
                        if (static_cast<int64_t>(lints.size()) > maxLintCount) return;
                    }
                }
            }
            for (auto& kv : *propsValidators) {
                JNodePtr value;
                if (!obj->TryGetValue(kv.first, value)) continue;
                kv.second(value, lints);
                if (static_cast<int64_t>(lints.size()) > maxLintCount) return;
            }
        };
    }
    if (dtype == DT_STR) {
        bool hasSpecialStringKeywords = false;
        RegexPtr regex;
        JNodePtr pattern;
        if (schema->TryGetValue("pattern", pattern)) {
            hasSpecialStringKeywords = true;
            regex = std::make_shared<Regex>(pattern->GetStr(), false, false);
        }
        JNodePtr minLengthNode, maxLengthNode;
        int64_t minLength = schema->TryGetValue("minLength", minLengthNode)
            ? minLengthNode->GetInt() : 0;
        int64_t maxLength = schema->TryGetValue("maxLength", maxLengthNode)
            ? maxLengthNode->GetInt() : INT64_MAX;
        if (minLength != 0 || maxLength != INT64_MAX) hasSpecialStringKeywords = true;
        if (hasSpecialStringKeywords) {
            return [regex, minLength, maxLength, typeMismatchLint](const JNodePtr& json,
                                                                   std::vector<JsonLint>& lints) {
                if (!json->IsStrValue()) {
                    lints.push_back(typeMismatchLint(json, DT_STR));
                    return;
                }
                const std::string& str = json->GetStr();
                if (regex && !regex->IsMatch(str)) {
                    lints.push_back(MakeSchemaLint(
                        JsonLintType::SCHEMA_STRING_DOESNT_MATCH_PATTERN,
                        "string '" + str + "' does not match regex '" + regex->Pattern() + "'",
                        json->position));
                }
                int64_t len = StrLenCodePoints(str);
                if (len < minLength) {
                    lints.push_back(MakeSchemaLint(JsonLintType::SCHEMA_STRING_TOO_SHORT,
                                                   "string " + str + " had less than minLength " +
                                                       std::to_string(minLength),
                                                   json->position));
                } else if (len > maxLength) {
                    lints.push_back(MakeSchemaLint(JsonLintType::SCHEMA_STRING_TOO_LONG,
                                                   "string " + str + " had greater than maxLength " +
                                                       std::to_string(maxLength),
                                                   json->position));
                }
            };
        }
    }
    if ((dtype & DT_FLOAT_OR_INT) != 0) {
        double inf = std::numeric_limits<double>::infinity();
        JNodePtr minNode, maxNode, exclMinNode, exclMaxNode;
        double minimum = schema->TryGetValue("minimum", minNode) ? minNode->ToDouble() : -inf;
        double maximum = schema->TryGetValue("maximum", maxNode) ? maxNode->ToDouble() : inf;
        double exclusiveMin = schema->TryGetValue("exclusiveMinimum", exclMinNode)
            ? exclMinNode->ToDouble() : -inf;
        double exclusiveMax = schema->TryGetValue("exclusiveMaximum", exclMaxNode)
            ? exclMaxNode->ToDouble() : inf;
        if (!std::isinf(minimum) || !std::isinf(maximum) || !std::isinf(exclusiveMin) ||
            !std::isinf(exclusiveMax)) {
            return [dtype, minimum, maximum, exclusiveMin, exclusiveMax,
                    typeMismatchLint](const JNodePtr& json, std::vector<JsonLint>& lints) {
                if (!TypeValidates(json->type, dtype)) {
                    lints.push_back(typeMismatchLint(json, dtype));
                    return;
                }
                double floatedValue = json->ToDouble();
                if (floatedValue < minimum)
                    lints.push_back(MakeSchemaLint(JsonLintType::SCHEMA_NUMBER_LESS_THAN_MIN,
                                                   "number " + CSharpNum(floatedValue) +
                                                       " less than minimum " + CSharpNum(minimum),
                                                   json->position));
                else if (floatedValue > maximum)
                    lints.push_back(MakeSchemaLint(JsonLintType::SCHEMA_NUMBER_GREATER_THAN_MAX,
                                                   "number " + CSharpNum(floatedValue) +
                                                       " greater than maximum " + CSharpNum(maximum),
                                                   json->position));
                if (floatedValue <= exclusiveMin)
                    lints.push_back(MakeSchemaLint(
                        JsonLintType::SCHEMA_NUMBER_LESSEQ_EXCLUSIVE_MIN,
                        "number " + CSharpNum(floatedValue) +
                            " less than or equal to exclusive minimum " + CSharpNum(exclusiveMin),
                        json->position));
                else if (floatedValue >= exclusiveMax)
                    lints.push_back(MakeSchemaLint(
                        JsonLintType::SCHEMA_NUMBER_GREATEREQ_EXCLUSIVE_MAX,
                        "number " + CSharpNum(floatedValue) +
                            " greater than or equal to exclusive maximum " + CSharpNum(exclusiveMax),
                        json->position));
            };
        }
    }
    return [dtype, typeMismatchLint](const JNodePtr& json, std::vector<JsonLint>& lints) {
        if (!TypeValidates(json->type, dtype)) lints.push_back(typeMismatchLint(json, dtype));
    };
}

} // namespace jsontools
