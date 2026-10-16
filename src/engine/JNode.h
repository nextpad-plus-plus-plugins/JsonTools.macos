// JNode.h — the JSON document model for the JsonTools macOS port.
//
// Faithful port of JsonToolsNppPlugin/JSONTools/JNode.cs (molsonkiko's
// JsonTools). Key representation differences from the C# original, chosen
// because this port is UTF-8-native:
//   * All strings are std::string holding UTF-8. JNode.position is therefore
//     simply a byte offset into the UTF-8 document — the C# original tracks
//     "extraUtf8_bytes" everywhere to convert UTF-16 string indices to UTF-8
//     positions; that bookkeeping disappears here by construction.
//   * Line-length decisions (PPrint's 79-column rule, maxLength truncation)
//     count BYTES here, UTF-16 code units in C#. Identical for ASCII; for
//     non-ASCII text the pprint line-break points can differ cosmetically.
//   * C# object references become std::shared_ptr<JNode>; RemesPath depends
//     on aliasing (MutateInto through a shared node), so value-semantic
//     containers are not an option.
//   * JNode::NL is a mutable static (default "\n"); the C# original hardcodes
//     "\r\n". The plugin shell sets it from the active document's EOL mode,
//     and the test harness sets "\r\n" when comparing against upstream
//     expected output.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace jsontools {

class JNode;
using JNodePtr = std::shared_ptr<JNode>;

// ── Dtype ──────────────────────────────────────────────────────────────────
enum Dtype : uint16_t {
    DT_TYPELESS = 0,
    DT_BOOL     = 1,
    DT_INT      = 2,
    DT_FLOAT    = 4,
    DT_STR      = 8,
    DT_NULL     = 16,
    DT_OBJ      = 32,
    DT_ARR      = 64,
    DT_UNKNOWN  = 128,
    DT_REGEX    = 256,
    DT_SLICE    = 512,
    DT_FUNCTION = 4096,
    // composites
    DT_FLOAT_OR_INT = DT_FLOAT | DT_INT,
    DT_INT_OR_BOOL  = DT_INT | DT_BOOL,
    DT_NUM          = DT_FLOAT | DT_INT | DT_BOOL,
    DT_ITERABLE     = DT_UNKNOWN | DT_ARR | DT_OBJ,
    DT_STR_OR_REGEX = DT_STR | DT_REGEX,
    DT_INT_OR_SLICE = DT_INT | DT_SLICE,
    DT_ARR_OR_OBJ   = DT_ARR | DT_OBJ,
    DT_SCALAR       = DT_FLOAT | DT_INT | DT_BOOL | DT_STR | DT_NULL | DT_REGEX,
    DT_ANYTHING     = DT_SCALAR | DT_ITERABLE,
};

enum class PrettyPrintStyle { Google, Whitesmith, PPrint };
enum class KeyStyle { JavaScript, Python, RemesPath };
enum class DocumentType { NONE, JSON, JSONL, INI, REGEX };

std::string FormatDtype(Dtype type);
inline bool BothTypesIntersect(Dtype a, Dtype b, Dtype shouldMatch) {
    return (a & shouldMatch) != 0 && (b & shouldMatch) != 0;
}

// ── exceptions ─────────────────────────────────────────────────────────────
struct JsonToolsError : std::runtime_error {
    explicit JsonToolsError(const std::string& m) : std::runtime_error(m) {}
};
struct ComparisonError : JsonToolsError { using JsonToolsError::JsonToolsError; };
struct InvalidMutationError : JsonToolsError { using JsonToolsError::JsonToolsError; };

// ── Regex wrapper ───────────────────────────────────────────────────────────
// Thin interface over NSRegularExpression (ICU) so the engine headers stay
// pure C++. Implemented in Regex.mm. Patterns use ICU syntax, which covers
// the .NET features JsonTools actually uses (named groups, lookaround,
// inline flags).
class Regex {
public:
    struct Group {
        bool matched = false;
        int64_t start = -1;   // byte offsets into the UTF-8 haystack
        int64_t length = 0;
        std::string value;
    };
    struct Match {
        bool success = false;
        int64_t start = -1;
        int64_t length = 0;
        std::string value;
        std::vector<Group> groups;      // groups[0] == whole match
        std::vector<std::string> groupNames; // parallel names ("" if unnamed)
    };

    // throws JsonToolsError on a bad pattern
    explicit Regex(const std::string& pattern, bool ignoreCase = false,
                   bool multiline = true);
    ~Regex();
    Regex(const Regex&) = delete;
    Regex& operator=(const Regex&) = delete;

    const std::string& Pattern() const { return pattern_; }
    bool IgnoreCase() const { return ignoreCase_; }
    bool IsMatch(const std::string& s) const;
    Match FirstMatch(const std::string& s, int64_t startAt = 0) const;
    std::vector<Match> Matches(const std::string& s) const;
    // .NET-style replacement with $1 / ${name} templates
    std::string Replace(const std::string& s, const std::string& repl) const;
    std::string Replace(const std::string& s,
                        const std::function<std::string(const Match&)>& fn) const;
    int GroupCount() const;                // not counting group 0
    std::vector<std::string> GroupNames() const;

private:
    void* impl_;          // NSRegularExpression*
    std::string pattern_;
    bool ignoreCase_;
};
using RegexPtr = std::shared_ptr<Regex>;

// ── Comment ────────────────────────────────────────────────────────────────
struct Comment {
    std::string content;
    bool isMultiline = false;
    int64_t position = 0;

    Comment() = default;
    Comment(std::string content_, bool isMultiline_, int64_t position_)
        : content(std::move(content_)), isMultiline(isMultiline_), position(position_) {}

    static int64_t ToStringHelper(std::string& sb, const Comment& c,
                                  const std::string& singleLineCommentStart);
    // returns new commentIdx; appends every comment with position <= position
    static size_t AppendAllCommentsBeforePosition(std::string& sb,
                                                  std::vector<Comment>& comments,
                                                  size_t commentIdx,
                                                  int64_t position,
                                                  const std::string& dent,
                                                  DocumentType commentType = DocumentType::JSON);
};

// ── ordered map (C# Dictionary preserves insertion order in practice) ──────
class OrderedMap {
public:
    using Item = std::pair<std::string, JNodePtr>;
    using iterator = std::vector<Item>::iterator;
    using const_iterator = std::vector<Item>::const_iterator;

    size_t size() const { return items_.size(); }
    bool empty() const { return items_.empty(); }
    iterator begin() { return items_.begin(); }
    iterator end() { return items_.end(); }
    const_iterator begin() const { return items_.begin(); }
    const_iterator end() const { return items_.end(); }

    bool contains(const std::string& k) const { return index_.count(k) != 0; }
    JNodePtr* find(const std::string& k);
    const JNodePtr* find(const std::string& k) const;
    // insert or overwrite (keeps original insertion position on overwrite)
    void set(const std::string& k, JNodePtr v);
    JNodePtr& operator[](const std::string& k); // inserts null JNodePtr if absent
    JNodePtr at(const std::string& k) const;    // throws std::out_of_range
    bool erase(const std::string& k);
    void clear() { items_.clear(); index_.clear(); }
    std::vector<std::string> keys() const;
    void reserve(size_t n) { items_.reserve(n); index_.reserve(n); }

private:
    std::vector<Item> items_;
    std::unordered_map<std::string, size_t> index_;
};

// ── scalar value storage ───────────────────────────────────────────────────
// index: 0 = null (monostate), 1 = bool, 2 = int64, 3 = double, 4 = string
using JValue = std::variant<std::monostate, bool, int64_t, double, std::string>;

// ── JNode ──────────────────────────────────────────────────────────────────
class JNode : public std::enable_shared_from_this<JNode> {
public:
    static constexpr int64_t PPRINT_LINE_LENGTH = 79;
    static constexpr int64_t I64_MAX = INT64_MAX;
    // Mutable, unlike the C# const "\r\n": the shell sets it per-document.
    static std::string NL;
    static constexpr char DEFAULT_PATH_SEPARATOR = '\x01';

    JValue value;         // monostate for arrays/objects/null
    Dtype type;
    int64_t position;     // byte offset in the UTF-8 source document

    JNode() : value(std::monostate{}), type(DT_NULL), position(0) {}
    JNode(JValue v, Dtype t, int64_t pos) : value(std::move(v)), type(t), position(pos) {}
    explicit JNode(int64_t v, int64_t pos = 0) : value(v), type(DT_INT), position(pos) {}
    explicit JNode(const std::string& v, int64_t pos = 0) : value(v), type(DT_STR), position(pos) {}
    explicit JNode(std::string&& v, int64_t pos = 0) : value(std::move(v)), type(DT_STR), position(pos) {}
    explicit JNode(double v, int64_t pos = 0) : value(v), type(DT_FLOAT), position(pos) {}
    explicit JNode(bool v, int64_t pos = 0) : value(v), type(DT_BOOL), position(pos) {}
    virtual ~JNode() = default;

    // convenience factories
    static JNodePtr Null(int64_t pos = 0) { return std::make_shared<JNode>(JValue{}, DT_NULL, pos); }
    static JNodePtr Int(int64_t v, int64_t pos = 0) { return std::make_shared<JNode>(v, pos); }
    static JNodePtr Float(double v, int64_t pos = 0) { return std::make_shared<JNode>(v, pos); }
    static JNodePtr Str(std::string v, int64_t pos = 0) { return std::make_shared<JNode>(std::move(v), pos); }
    static JNodePtr Bool(bool v, int64_t pos = 0) { return std::make_shared<JNode>(v, pos); }

    // scalar accessors (undefined behavior if wrong type — callers check type)
    bool GetBool() const { return std::get<bool>(value); }
    int64_t GetInt() const { return std::get<int64_t>(value); }
    double GetDouble() const { return std::get<double>(value); }
    const std::string& GetStr() const { return std::get<std::string>(value); }
    bool IsStrValue() const { return std::holds_alternative<std::string>(value); }
    // numeric value of bool/int/float (throws ComparisonError otherwise)
    double ToDouble() const;

    // ── string building primitives ──
    static void CharToSb(std::string& sb, unsigned char c);
    static std::string StrToString(const std::string& s, bool quoted);
    static void StrToSb(std::string& sb, const std::string& s);
    static std::string DoubleToString(double d); // C# double.ToString("R") rules
    // implemented in JsonParser.cpp (needs the parser):
    static std::string UnescapedJsonString(const std::string& str, bool strAlreadyQuoted);

    // ── compact printing ──
    virtual std::string ToString(bool sortKeys = true,
                                 const std::string& keyValueSep = ": ",
                                 const std::string& itemSep = ", ",
                                 int64_t maxLength = I64_MAX) const;
    std::string ValueOrToString() const {
        return IsStrValue() ? GetStr() : ToString();
    }
    // returns -1 when sb crossed maxLength (truncation sentinel, mirrors C#)
    virtual int ToStringHelper(bool sortKeys, const std::string& keyValueSep,
                               const std::string& itemSep, std::string& sb,
                               bool changePositions, int64_t maxLength);

    // ── pretty printing ──
    virtual std::string PrettyPrint(int indent = 4, bool sortKeys = true,
                                    PrettyPrintStyle style = PrettyPrintStyle::Google,
                                    int64_t maxLength = I64_MAX, char indentChar = ' ');
    virtual int PrettyPrintHelper(int indent, bool sortKeys, PrettyPrintStyle style,
                                  int depth, std::string& sb, bool changePositions,
                                  int64_t maxLength, char indentChar);
    virtual std::string ToStringAndChangePositions(bool sortKeys = true,
                                                   const std::string& keyValueSep = ": ",
                                                   const std::string& itemSep = ", ",
                                                   int64_t maxLength = I64_MAX);
    virtual std::string PrettyPrintAndChangePositions(int indent = 4, bool sortKeys = true,
                                                      PrettyPrintStyle style = PrettyPrintStyle::Google,
                                                      int64_t maxLength = I64_MAX,
                                                      char indentChar = ' ');
    virtual int PPrintHelper(int indent, int depth, bool sortKeys, std::string& sb,
                             bool changePositions, int64_t maxLineEnd,
                             int64_t maxLength, char indentChar);

    // ── printing with comments ──
    std::string ToStringWithComments(std::vector<Comment>& comments, bool sortKeys = true);
    std::string ToStringWithCommentsAndChangePositions(std::vector<Comment>& comments,
                                                       bool sortKeys = true);
    std::string PrettyPrintWithComments(std::vector<Comment>& comments, int indent = 4,
                                        bool sortKeys = true, char indentChar = ' ',
                                        PrettyPrintStyle prettyPrintStyle = PrettyPrintStyle::Google);
    std::string PrettyPrintWithCommentsAndChangePositions(std::vector<Comment>& comments,
                                                          int indent = 4, bool sortKeys = true,
                                                          char indentChar = ' ',
                                                          PrettyPrintStyle prettyPrintStyle = PrettyPrintStyle::Google);
    virtual size_t PrettyPrintWithCommentsHelper(std::vector<Comment>& comments,
                                                 size_t commentIdx, int indent, bool sortKeys,
                                                 int depth, std::string& sb,
                                                 bool changePositions, char indentChar,
                                                 bool pprint);
    virtual size_t PPrintWithCommentsHelper(std::vector<Comment>& comments, size_t commentIdx,
                                            int indent, bool sortKeys, int depth,
                                            std::string& sb, bool changePositions,
                                            char indentChar, int64_t maxSbLen);
    virtual int64_t CompressedLengthAndPositionsWithinThresholds(int64_t sbLength,
                                                                 int64_t maxInitialPosition,
                                                                 int64_t maxSbLen) const;

    // ── comparison / equality / copying ──
    // negative / 0 / positive like C# CompareTo; throws ComparisonError on
    // type mismatches exactly where the C# throws ArgumentException.
    int CompareTo(const JNode& other) const;
    int CompareTo(const JValue& other) const;
    virtual bool Equals(const JNode& other) const;
    bool TryEquals(const JNode& other, std::string* errorMessage = nullptr) const;
    virtual JNodePtr Copy() const;

    // ── tree navigation ──
    struct PathStep {
        bool isIndex = false;
        int64_t index = 0;
        std::string key;
    };
    // (keyInParent, parent) pairs from root down to this node's parent
    std::vector<std::pair<PathStep, JNodePtr>> ParentHierarchy(const JNodePtr& root);
    bool ContainsPosition(int64_t pos) const;
    std::string PathToPosition(int64_t pos, KeyStyle style = KeyStyle::Python,
                               char separator = DEFAULT_PATH_SEPARATOR);
    static std::string FormatKey(const std::string& key, KeyStyle style = KeyStyle::Python,
                                 char separator = DEFAULT_PATH_SEPARATOR);
    static std::string FormatIndex(int64_t idx, char separator);
    static void ThrowIfPathSeparatorInvalid(char sep);
    static bool DotCompatible(const std::string& key);

    // ── RemesPath hooks ──
    virtual bool IsMutator() const { return false; }
    virtual bool CanOperate() const { return false; }
    virtual JNodePtr Operate(JNodePtr /*inp*/) {
        throw JsonToolsError("This type of JNode cannot operate on input");
    }
    void MutateInto(const JNode& vnew);

private:
    std::string PathToPositionHelper(int64_t pos, KeyStyle style,
                                     std::vector<PathStep>& path, char separator);
    bool ParentHierarchyHelper(const JNodePtr& current, const JNode* target,
                               std::vector<PathStep>& keys, std::vector<JNodePtr>& parents);
};

// key comparator used wherever C# sorts keys (CurrentCultureIgnoreCase):
// case-insensitive ASCII compare, ordinal tiebreak (deterministic).
bool KeyLessIgnoreCase(const std::string& a, const std::string& b);

// ── JObject ────────────────────────────────────────────────────────────────
class JObject : public JNode {
public:
    OrderedMap children;

    JObject() : JNode(JValue{}, DT_OBJ, 0) {}
    explicit JObject(int64_t pos) : JNode(JValue{}, DT_OBJ, pos) {}

    size_t Length() const { return children.size(); }
    JNodePtr& operator[](const std::string& key) { return children[key]; }
    JNodePtr at(const std::string& key) const { return children.at(key); }
    bool TryGetValue(const std::string& key, JNodePtr& val) const;
    bool TryGetString(const std::string& key, std::string& val) const;

    std::string ToString(bool sortKeys = true, const std::string& keyValueSep = ": ",
                         const std::string& itemSep = ", ",
                         int64_t maxLength = I64_MAX) const override;
    int ToStringHelper(bool sortKeys, const std::string& keyValueSep,
                       const std::string& itemSep, std::string& sb,
                       bool changePositions, int64_t maxLength) override;
    std::string PrettyPrint(int indent = 4, bool sortKeys = true,
                            PrettyPrintStyle style = PrettyPrintStyle::Google,
                            int64_t maxLength = I64_MAX, char indentChar = ' ') override;
    int PrettyPrintHelper(int indent, bool sortKeys, PrettyPrintStyle style, int depth,
                          std::string& sb, bool changePositions, int64_t maxLength,
                          char indentChar) override;
    std::string ToStringAndChangePositions(bool sortKeys = true,
                                           const std::string& keyValueSep = ": ",
                                           const std::string& itemSep = ", ",
                                           int64_t maxLength = I64_MAX) override;
    std::string PrettyPrintAndChangePositions(int indent = 4, bool sortKeys = true,
                                              PrettyPrintStyle style = PrettyPrintStyle::Google,
                                              int64_t maxLength = I64_MAX,
                                              char indentChar = ' ') override;
    int PPrintHelper(int indent, int depth, bool sortKeys, std::string& sb,
                     bool changePositions, int64_t maxLineEnd, int64_t maxLength,
                     char indentChar) override;
    size_t PrettyPrintWithCommentsHelper(std::vector<Comment>& comments, size_t commentIdx,
                                         int indent, bool sortKeys, int depth, std::string& sb,
                                         bool changePositions, char indentChar,
                                         bool pprint) override;
    size_t PPrintWithCommentsHelper(std::vector<Comment>& comments, size_t commentIdx,
                                    int indent, bool sortKeys, int depth, std::string& sb,
                                    bool changePositions, char indentChar,
                                    int64_t maxSbLen) override;
    int64_t CompressedLengthAndPositionsWithinThresholds(int64_t sbLength,
                                                         int64_t maxInitialPosition,
                                                         int64_t maxSbLen) const override;
    bool Equals(const JNode& other) const override;
    JNodePtr Copy() const override;

    void StringifyAllValuesInIniFile();
    std::string ToIniFile(std::vector<Comment>& comments);
};

// ── JArray ─────────────────────────────────────────────────────────────────
class JArray : public JNode {
public:
    std::vector<JNodePtr> children;

    JArray() : JNode(JValue{}, DT_ARR, 0) {}
    explicit JArray(int64_t pos) : JNode(JValue{}, DT_ARR, pos) {}

    size_t Length() const { return children.size(); }
    JNodePtr& operator[](size_t i) { return children[i]; }
    const JNodePtr& operator[](size_t i) const { return children[i]; }

    std::string ToString(bool sortKeys = true, const std::string& keyValueSep = ": ",
                         const std::string& itemSep = ", ",
                         int64_t maxLength = I64_MAX) const override;
    int ToStringHelper(bool sortKeys, const std::string& keyValueSep,
                       const std::string& itemSep, std::string& sb,
                       bool changePositions, int64_t maxLength) override;
    std::string PrettyPrint(int indent = 4, bool sortKeys = true,
                            PrettyPrintStyle style = PrettyPrintStyle::Google,
                            int64_t maxLength = I64_MAX, char indentChar = ' ') override;
    int PrettyPrintHelper(int indent, bool sortKeys, PrettyPrintStyle style, int depth,
                          std::string& sb, bool changePositions, int64_t maxLength,
                          char indentChar) override;
    std::string ToStringAndChangePositions(bool sortKeys = true,
                                           const std::string& keyValueSep = ": ",
                                           const std::string& itemSep = ", ",
                                           int64_t maxLength = I64_MAX) override;
    std::string PrettyPrintAndChangePositions(int indent = 4, bool sortKeys = true,
                                              PrettyPrintStyle style = PrettyPrintStyle::Google,
                                              int64_t maxLength = I64_MAX,
                                              char indentChar = ' ') override;
    int PPrintHelper(int indent, int depth, bool sortKeys, std::string& sb,
                     bool changePositions, int64_t maxLineEnd, int64_t maxLength,
                     char indentChar) override;
    size_t PrettyPrintWithCommentsHelper(std::vector<Comment>& comments, size_t commentIdx,
                                         int indent, bool sortKeys, int depth, std::string& sb,
                                         bool changePositions, char indentChar,
                                         bool pprint) override;
    size_t PPrintWithCommentsHelper(std::vector<Comment>& comments, size_t commentIdx,
                                    int indent, bool sortKeys, int depth, std::string& sb,
                                    bool changePositions, char indentChar,
                                    int64_t maxSbLen) override;
    int64_t CompressedLengthAndPositionsWithinThresholds(int64_t sbLength,
                                                         int64_t maxInitialPosition,
                                                         int64_t maxSbLen) const override;
    bool Equals(const JNode& other) const override;
    JNodePtr Copy() const override;

    std::string ToJsonLines(bool sortKeys = true, const std::string& keyValueSep = ": ",
                            const std::string& itemSep = ", ");
};

// ── RemesPath helper node types ────────────────────────────────────────────
class JRegex : public JNode {
public:
    RegexPtr regex;
    explicit JRegex(RegexPtr r) : JNode(JValue{}, DT_REGEX, 0), regex(std::move(r)) {}
    std::string ToString(bool sortKeys = true, const std::string& keyValueSep = ": ",
                         const std::string& itemSep = ", ",
                         int64_t maxLength = I64_MAX) const override;
    JNodePtr Copy() const override { return std::make_shared<JRegex>(regex); }
};

class JSlicer : public JNode {
public:
    // 1-3 nullable ints (start, stop, stride)
    std::vector<std::pair<bool, int64_t>> slicer; // (hasValue, value)
    explicit JSlicer(std::vector<std::pair<bool, int64_t>> s)
        : JNode(JValue{}, DT_SLICE, 0), slicer(std::move(s)) {}
};

class CurJson : public JNode {
public:
    std::function<JNodePtr(JNodePtr)> function;

    CurJson() : JNode(JValue{}, DT_UNKNOWN, 0), function(Identity) {}
    CurJson(Dtype t, std::function<JNodePtr(JNodePtr)> fn)
        : JNode(JValue{}, static_cast<Dtype>(t), 0), function(std::move(fn)) {}

    static JNodePtr Identity(JNodePtr obj) { return obj; }
    bool CanOperate() const override { return true; }
    JNodePtr Operate(JNodePtr inp) override { return function(std::move(inp)); }
    std::string ToString(bool = true, const std::string& = ": ", const std::string& = ", ",
                         int64_t = I64_MAX) const override {
        return "CurJson(type = " + FormatDtype(type) + ")";
    }
};

class JMutator : public JNode {
public:
    JNodePtr selector;
    JNodePtr mutator;

    JMutator(JNodePtr sel, JNodePtr mut)
        : JNode(JValue{}, DT_UNKNOWN, 0), selector(std::move(sel)), mutator(std::move(mut)) {}
    bool CanOperate() const override { return true; }
    bool IsMutator() const override { return true; }
    JNodePtr Operate(JNodePtr inp) override;
    std::string ToString(bool = true, const std::string& = ": ", const std::string& = ", ",
                         int64_t = I64_MAX) const override {
        return "JMutator(selector = " + selector->ToString() + ", mutator = " + mutator->ToString() + ")";
    }
};

enum class VariableAssignmentType { INVALID, NORMAL, LOOP };

class VarAssign : public JNode {
public:
    std::string Name;
    VariableAssignmentType AssignmentType;
    int IndexOfDeclaration;
    int64_t IndexOfLoopEnd = I64_MAX;
    JNodePtr OriginalValue;
    bool IsFunctionOfInput;
    JNodePtr varValue; // the C# stores this in JNode.value; we keep it typed

    VarAssign(std::string name, JNodePtr json, int indexOfDeclaration = 0,
              VariableAssignmentType assignmentType = VariableAssignmentType::NORMAL,
              bool isFunctionOfInput = false);
    JNodePtr GetInitialValueOfVariable(bool isCompiling);
};

class LoopEnd : public JNode {
public:
    LoopEnd() : JNode() {}
    std::string ToString(bool = true, const std::string& = ": ", const std::string& = ", ",
                         int64_t = I64_MAX) const override { return "LoopEnd()"; }
};

struct RemesPathNameException : JsonToolsError {
    RemesPathNameException(const std::string& varname, int64_t indexInStatements,
                           int64_t indexOfDeclaration)
        : JsonToolsError("Variable named \"" + varname + "\" was referenced in statement " +
                         std::to_string(indexInStatements + 1) +
                         ", but was not declared until statement " +
                         std::to_string(indexOfDeclaration + 1)) {}
};
struct RemesPathLoopVarNotAnArrayException : JsonToolsError {
    RemesPathLoopVarNotAnArrayException(const VarAssign& va, Dtype gotType)
        : JsonToolsError("Loop variable \"" + va.Name + "\" must be an array, got type " +
                         FormatDtype(gotType)) {}
};

class JQueryContext : public JNode {
public:
    JQueryContext() : JNode(JValue{}, DT_UNKNOWN, 0) {}

    int64_t indexInStatements = 0;
    bool CanOperate() const override { return true; }
    bool IsMutator() const override { return mutatesInput_; }
    JNodePtr Operate(JNodePtr inp) override;
    bool IsSimpleQuery() const;
    std::vector<std::string> Varnames() const;
    void AddStatement(JNodePtr statement);
    bool TryGetValue(int64_t tokenIndex, const std::string& varname, JNodePtr& val);
    bool AnyVariableReferencedInRange(int64_t startIndex, int64_t endIndex) const;
    JNodePtr GetQuery(); // `this` must be owned by a shared_ptr

private:
    bool mutatesInput_ = false;
    std::vector<JNodePtr> statements_;
    OrderedMap locals_;                       // null JNodePtr = declared-later placeholder
    std::unordered_map<std::string, JNodePtr> cachedLocals_;
    std::vector<std::shared_ptr<VarAssign>> loopVariableAssignmentStack_;
    std::unordered_map<int64_t, std::string> tokenIndicesOfVariableReferences_;

    JNodePtr AssignVariable(const std::shared_ptr<VarAssign>& va, JNodePtr inp);
    JNodePtr EvaluateForLoop(const std::shared_ptr<VarAssign>& va, JNodePtr inp);
    JNodePtr EvaluateStatementsFromStartToEnd(JNodePtr inp, int64_t start, int64_t end);
    void Reset();
};

// downcast helpers (nullptr when wrong type), mirroring C# `is` patterns
inline JObject* AsObj(const JNodePtr& n) { return n && n->type == DT_OBJ ? static_cast<JObject*>(n.get()) : nullptr; }
inline JArray* AsArr(const JNodePtr& n) { return n && n->type == DT_ARR ? static_cast<JArray*>(n.get()) : nullptr; }
inline JObject* AsObj(JNode* n) { return n && n->type == DT_OBJ ? static_cast<JObject*>(n) : nullptr; }
inline JArray* AsArr(JNode* n) { return n && n->type == DT_ARR ? static_cast<JArray*>(n) : nullptr; }

} // namespace jsontools
