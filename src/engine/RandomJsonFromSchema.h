// RandomJsonFromSchema.h — generate random JSON matching a (display-form)
// JSON schema. Port of RandomJsonFromSchema.cs.
//
// Port note: the C# can generate random strings matching a schema's
// "pattern"/"patternProperties" regexes (via RandomStringFromRegex). This
// port does not yet include that generator; patterns are silently ignored,
// which is exactly how the C# behaves when its generator rejects a pattern.

#pragma once

#include "JNode.h"

namespace jsontools {

struct SchemaValidationException : JsonToolsError {
    using JsonToolsError::JsonToolsError;
};

class RandomJsonFromSchema {
public:
    static constexpr int RECURSION_LIMIT = 64; // JsonSchemaValidator.RECURSION_LIMIT

    static JNodePtr RandomJson(const JNodePtr& schema, int64_t minArrayLength,
                               int64_t maxArrayLength, bool extendedAsciiStrings,
                               bool usePatterns);

private:
    int64_t minArrayLength_;
    int64_t maxArrayLength_;
    bool extendedAsciiStrings_;
    bool usePatterns_;

    RandomJsonFromSchema(int64_t minA, int64_t maxA, bool ext, bool pat)
        : minArrayLength_(minA), maxArrayLength_(maxA), extendedAsciiStrings_(ext),
          usePatterns_(pat) {}

    JNodePtr Helper(const JNodePtr& schema, JObject* refs, int recursionDepth);
    JNodePtr RandomBoolean();
    JNodePtr RandomFloat();
    JNodePtr RandomIntNode();
    JNodePtr RandomNumber();
    JNodePtr RandomString(const JNodePtr& schema);
    JNodePtr RandomArray(const JNodePtr& schema, JObject* refs, int recursionDepth);
    JNodePtr RandomObject(const JNodePtr& schema, JObject* refs, int recursionDepth);
    JNodePtr RandomAnything(int recursionDepth);
    JNodePtr Dispatch(const std::string& type, const JNodePtr& schema, JObject* refs,
                      int recursionDepth);
};

} // namespace jsontools
