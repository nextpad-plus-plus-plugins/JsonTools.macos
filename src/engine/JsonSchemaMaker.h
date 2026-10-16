// JsonSchemaMaker.h — minimalist JSON schema generation (JsonSchemaMaker.cs).
//
// Port note: the C# represents in-progress schemas as Dictionary<string,
// object> where "type" holds a raw Dtype enum and "required" holds a
// HashSet<string>. This port uses a JNode tree with the same shape, where
// "type" is an INT JNode holding the Dtype bits and "required" is a string
// array. SchemaToJNode converts that internal form to the display form with
// type-name strings.

#pragma once

#include "JNode.h"

namespace jsontools {

class JsonSchemaMaker {
public:
    static std::string TypeName(Dtype x);
    static Dtype DtypeUnion(Dtype a, Dtype b);
    static bool IsSingleDtype(Dtype x);
    // internal-form schema (see header comment)
    static JNodePtr BuildSchema(const JNodePtr& json);
    static JNodePtr MergeSchemas(JNodePtr s1, JNodePtr s2);
    // internal form → display form (type names, sorted anyOf/required)
    static JNodePtr SchemaToJNode(const JNodePtr& schema);
    // display-form schema for json, including the $schema key
    static JNodePtr GetSchema(const JNodePtr& json);

    static const std::unordered_map<std::string, Dtype>& TypeNameToDtype();
};

} // namespace jsontools
