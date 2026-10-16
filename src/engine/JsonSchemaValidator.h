// JsonSchemaValidator.h — compile JSON schemas (display form) into validation
// functions. Port of JsonSchemaValidator.cs.

#pragma once

#include "JNode.h"
#include "JsonParser.h"

namespace jsontools {

class JsonSchemaValidator {
public:
    static constexpr int RECURSION_LIMIT = 64;

    using ValidationHelperFunc = std::function<void(const JNodePtr&, std::vector<JsonLint>&)>;
    using ValidationFunc = std::function<bool(const JNodePtr&, std::vector<JsonLint>&)>;

    static ValidationFunc CompileValidationFunc(const JNodePtr& schema, int64_t maxLintCount);
    static ValidationHelperFunc CompileValidationHelperFunc(const JNodePtr& schema,
                                                            const std::shared_ptr<JObject>& definitions,
                                                            int recursions, int64_t maxLintCount);
    static bool Validates(const JNodePtr& json, const JNodePtr& schema, int64_t maxLintCount,
                          std::vector<JsonLint>& lints);
    static std::string LintsAsJArrayString(const std::vector<JsonLint>& lints);
    static bool TypeValidates(Dtype jsonType, Dtype schemaType) {
        return jsonType == schemaType || (jsonType == DT_INT && schemaType == DT_FLOAT);
    }
};

} // namespace jsontools
