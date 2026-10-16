// JsonParser.h — JSON/JSON5 parser + linter for the JsonTools macOS port.
// Faithful port of JsonToolsNppPlugin/JSONTools/JsonParser.cs. The port is
// UTF-8-native: `ii` is a byte offset into the UTF-8 document, so the C#
// utf8ExtraBytes machinery vanishes (utf8Pos == ii). Lint positions are byte
// offsets, exactly like upstream's "position in the UTF-8 encoding".

#pragma once

#include "JNode.h"

namespace jsontools {

enum class ParserState {
    STRICT, OK, NAN_INF, JSONC, JSON5,
    BAD,    // always-logged recoverable errors
    FATAL,  // always-fatal errors
    SCHEMA, // reserved for JSON Schema validation errors
};

// errors above this level are reported by a JsonParser
enum class LoggerLevel { STRICT, OK, NAN_INF, JSONC, JSON5 };

// Tiered: ParserState of JsonLintType x is 1 + (x >> 10).
enum class JsonLintType : int16_t {
    OK_CONTROL_CHAR = 0,
    NAN_INF_Infinity = (static_cast<int>(ParserState::NAN_INF) - 1) << 10,
    NAN_INF_NaN,
    JSONC_JAVASCRIPT_COMMENT = (static_cast<int>(ParserState::JSONC) - 1) << 10,
    JSON5_WHITESPACE_CHAR = (static_cast<int>(ParserState::JSON5) - 1) << 10,
    JSON5_SINGLEQUOTED_STRING,
    JSON5_ESCAPED_NEWLINE,
    JSON5_X_ESCAPE,
    JSON5_ESCAPED_CHAR,            // param1 = nextChar
    JSON5_UNQUOTED_KEY,
    JSON5_NUM_LEADING_PLUS,
    JSON5_HEX_NUM,
    JSON5_NUM_LEADING_DECIMAL_POINT,
    JSON5_NUM_TRAILING_DECIMAL_POINT,
    JSON5_COMMA_AFTER_LAST_ELEMENT_ARRAY,
    JSON5_COMMA_AFTER_LAST_ELEMENT_OBJECT,
    BAD_UNTERMINATED_MULTILINE_COMMENT = (static_cast<int>(ParserState::BAD) - 1) << 10,
    BAD_PYTHON_COMMENT,
    BAD_STRING_CONTAINS_NEWLINE,
    BAD_UNTERMINATED_STRING,       // param1 = startUtf8Pos
    BAD_KEY_CONTAINS_NEWLINE,
    BAD_INVALID_UNQUOTED_KEY,      // param1 = startPosOfKey
    BAD_PYTHON_nan,
    BAD_PYTHON_None,
    BAD_PYTHON_inf,
    BAD_UNNECESSARY_LEADING_0,
    BAD_SLASH_FRACTION,
    BAD_NUMBER_INVALID_FORMAT,     // param1 = numStr
    BAD_TWO_CONSECUTIVE_COMMAS_ARRAY,   // param1 = positionInArr
    BAD_COMMA_BEFORE_FIRST_ELEMENT_ARRAY,
    BAD_ARRAY_ENDSWITH_CURLYBRACE,
    BAD_NO_COMMA_BETWEEN_ARRAY_ITEMS,
    BAD_COLON_BETWEEN_ARRAY_ITEMS,
    BAD_UNTERMINATED_ARRAY,
    BAD_TWO_CONSECUTIVE_COMMAS_OBJECT,  // param1 = positionInObj
    BAD_COMMA_BEFORE_FIRST_PAIR_OBJECT,
    BAD_OBJECT_ENDSWITH_SQUAREBRACE,
    BAD_NO_COMMA_BETWEEN_OBJECT_PAIRS,  // param1 = positionInObj
    BAD_COMMA_AFTER_OBJECT_KEY,
    BAD_UNTERMINATED_OBJECT,
    BAD_COLON_BETWEEN_OBJECT_PAIRS,
    BAD_CHAR_WHERE_COLON_EXPECTED,      // param1 = c, param2 = childCount
    BAD_NO_COLON_BETWEEN_OBJECT_KEY_VALUE, // param1 = childCount
    BAD_DUPLICATE_KEY,                  // param1 = key
    BAD_PYTHON_True,
    BAD_PYTHON_False,
    BAD_JAVASCRIPT_undefined,
    BAD_CHAR_INSTEAD_OF_EOF,            // param1 = c
    BAD_FLOAT_TOO_LARGE,                // param1 = numStr
    FATAL_EXPECTED_JAVASCRIPT_COMMENT = (static_cast<int>(ParserState::FATAL) - 1) << 10,
    FATAL_HEXADECIMAL_TOO_SHORT,        // param1 = expected_hex_length
    FATAL_UNTERMINATED_KEY = FATAL_EXPECTED_JAVASCRIPT_COMMENT + 3,
    FATAL_INVALID_STARTSWITH_n,
    FATAL_PLUS_OR_MINUS_AT_EOF,         // param1 = lastCharOfDoc
    FATAL_INVALID_STARTSWITH_I,
    FATAL_INVALID_STARTSWITH_N,
    FATAL_INVALID_STARTSWITH_i,
    FATAL_HEX_INT_OVERFLOW,
    FATAL_SECOND_DECIMAL_POINT,
    FATAL_NUM_TRAILING_e_OR_E,
    FATAL_MAX_RECURSION_DEPTH,
    FATAL_UNEXPECTED_EOF,
    FATAL_NO_VALID_LITERAL_POSSIBLE,
    FATAL_INVALID_STARTSWITH_t,
    FATAL_INVALID_STARTSWITH_f,
    FATAL_INVALID_STARTSWITH_T,
    FATAL_INVALID_STARTSWITH_F,
    FATAL_INVALID_STARTSWITH_u,
    FATAL_BADLY_LOCATED_CHAR,           // param1 = charStr
    FATAL_NO_INPUT,
    FATAL_ONLY_WHITESPACE_COMMENTS,
    FATAL_JSONL_NOT_ONE_DOC_PER_LINE,
    FATAL_UNSPECIFIED_ERROR = FATAL_EXPECTED_JAVASCRIPT_COMMENT + 25, // param1 = errorMessage
    SCHEMA_TYPE_MISMATCH = (static_cast<int>(ParserState::SCHEMA) - 1) << 10, // all SCHEMA_*: param1 = errorMessage
    SCHEMA_TYPE_ARRAY_MISMATCH,
    SCHEMA_VALUE_NOT_IN_ENUM,
    SCHEMA_ARRAY_TOO_LONG,
    SCHEMA_ARRAY_TOO_SHORT,
    SCHEMA_CONTAINS_VIOLATION,
    SCHEMA_OBJECT_MISSING_REQUIRED_KEY,
    SCHEMA_FALSE_SCHEMA,
    SCHEMA_STRING_DOESNT_MATCH_PATTERN,
    SCHEMA_RECURSION_LIMIT_REACHED,
    SCHEMA_NUMBER_LESS_THAN_MIN,
    SCHEMA_NUMBER_GREATER_THAN_MAX,
    SCHEMA_NUMBER_LESSEQ_EXCLUSIVE_MIN,
    SCHEMA_NUMBER_GREATEREQ_EXCLUSIVE_MAX,
    SCHEMA_STRING_TOO_LONG,
    SCHEMA_STRING_TOO_SHORT,
    SCHEMA_MINCONTAINS_VIOLATION,
};

using LintParam = std::variant<std::monostate, int64_t, char, std::string>;

struct JsonLint {
    JsonLintType lintType = JsonLintType::OK_CONTROL_CHAR;
    int64_t pos = 0;      // byte position in the UTF-8 document
    char curChar = '\0';  // byte at the position where the error began
    LintParam param1;
    LintParam param2;

    JsonLint() = default;
    JsonLint(JsonLintType t, int64_t pos_, char c, LintParam p1 = {}, LintParam p2 = {})
        : lintType(t), pos(pos_), curChar(c), param1(std::move(p1)), param2(std::move(p2)) {}

    ParserState severity() const {
        return static_cast<ParserState>((static_cast<int>(lintType) >> 10) + 1);
    }
    std::string message() const;
    std::string ToString() const;
    static std::string CharDisplay(char c);
    static std::string ParserStateName(ParserState s);
    JNodePtr ToJson() const; // {"message":..., "position":..., "severity":...}
};

struct JsonParserException : JsonToolsError {
    JsonLint lint;
    char curChar;
    JsonParserException(const JsonLint& l, char c)
        : JsonToolsError(l.message() + " at position " + std::to_string(l.pos) +
                         " (char " + JsonLint::CharDisplay(c) + ")"),
          lint(l), curChar(c) {}
};

class JsonParser {
public:
    static constexpr int MAX_RECURSION_DEPTH = 512;

    std::vector<JsonLint> lint;
    int64_t ii = 0;                 // byte position in the document
    std::vector<Comment> comments;  // only populated when rememberComments
    LoggerLevel loggerLevel;
    bool throwIfLogged;
    bool throwIfFatal;

    explicit JsonParser(LoggerLevel loggerLevel_ = LoggerLevel::NAN_INF,
                        bool throwIfLogged_ = true, bool throwIfFatal_ = true,
                        bool rememberComments_ = false)
        : loggerLevel(loggerLevel_), throwIfLogged(throwIfLogged_),
          throwIfFatal(throwIfFatal_), rememberComments(rememberComments_) {}

    bool rememberComments = false;
    ParserState state = ParserState::STRICT;

    int64_t utf8Pos() const { return ii; }
    bool fatal() const { return state == ParserState::FATAL; }
    bool hasLogged() const { return static_cast<int>(state) > static_cast<int>(loggerLevel); }
    bool exitedEarly() const { return fatal() || (throwIfLogged && hasLogged()); }
    const JsonLint* fatalError() const {
        return exitedEarly() && !lint.empty() ? &lint.back() : nullptr;
    }

    JNodePtr Parse(const std::string& inp);
    JNodePtr ParseJsonLines(const std::string& inp);
    JNodePtr ParseSomething(const std::string& inp, int recursionDepth);
    JNodePtr ParseString(const std::string& inp);
    JNodePtr ParseNumber(const std::string& inp);
    std::shared_ptr<JArray> ParseArray(const std::string& inp, int recursionDepth);
    std::shared_ptr<JObject> ParseObject(const std::string& inp, int recursionDepth);
    // returns false on failure; key output in `key`
    bool ParseKey(const std::string& inp, std::string& key);

    void Reset();
    JsonParser Copy() const { return JsonParser(loggerLevel, throwIfLogged, throwIfFatal); }

    static JNodePtr TryParseNumber(const std::string& inp, int64_t start, int64_t end,
                                   int64_t jnodePosition);
    static std::string SubstringUnlessAll(const std::string& inp, int64_t start, int64_t end) {
        return (start == 0 && end == static_cast<int64_t>(inp.size()))
            ? inp : inp.substr(static_cast<size_t>(start), static_cast<size_t>(end - start));
    }
    static int64_t EndOfPreviousLine(const std::string& inp, int64_t ii, int64_t start);

private:
    bool HandleError(JsonLintType lintType, const std::string& inp, int64_t pos,
                     LintParam param1 = {}, LintParam param2 = {});
    void ConsumeLine(const std::string& inp);
    bool ConsumeInsignificantChars(const std::string& inp);
    int ParseHexChar(const std::string& inp, int length);
    bool HandleCharErrors(int64_t c, const std::string& inp, int64_t pos);
    bool ParseUnquotedKey(const std::string& inp, std::string& key);
    // appends UTF-16 code unit `unit` to UTF-8 string sb, combining a pending
    // high surrogate; returns pending high surrogate (0 = none)
    static void AppendCodeUnit(std::string& sb, int unit, int& pendingHighSurrogate);
    static void FlushPendingSurrogate(std::string& sb, int& pendingHighSurrogate);
};

// append a unicode code point (NOT a UTF-16 unit) as UTF-8
void AppendCodePoint(std::string& sb, uint32_t cp);
// decode the UTF-8 code point starting at byte i; advances len to its byte length
uint32_t DecodeCodePoint(const std::string& s, size_t i, size_t& len);

} // namespace jsontools
