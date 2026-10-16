// JsonParser.cpp — see JsonParser.h for the port notes.

#include "JsonParser.h"

#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <xlocale.h>

namespace jsontools {

static const double kNan = std::numeric_limits<double>::quiet_NaN();
static const double kInf = std::numeric_limits<double>::infinity();

// ── UTF-8 primitives ───────────────────────────────────────────────────────
void AppendCodePoint(std::string& sb, uint32_t cp) {
    if (cp < 0x80) {
        sb += static_cast<char>(cp);
    } else if (cp < 0x800) {
        sb += static_cast<char>(0xC0 | (cp >> 6));
        sb += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        // NOTE: lone surrogates (0xD800-0xDFFF) are deliberately encoded
        // WTF-8 style, mirroring how the C# original keeps lone UTF-16
        // surrogates in its strings.
        sb += static_cast<char>(0xE0 | (cp >> 12));
        sb += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        sb += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        sb += static_cast<char>(0xF0 | (cp >> 18));
        sb += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        sb += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        sb += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

uint32_t DecodeCodePoint(const std::string& s, size_t i, size_t& len) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) { len = 1; return c; }
    if ((c >> 5) == 0x6 && i + 1 < s.size()) {
        len = 2;
        return (static_cast<uint32_t>(c & 0x1F) << 6) |
               (static_cast<unsigned char>(s[i + 1]) & 0x3F);
    }
    if ((c >> 4) == 0xE && i + 2 < s.size()) {
        len = 3;
        return (static_cast<uint32_t>(c & 0x0F) << 12) |
               ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 6) |
               (static_cast<unsigned char>(s[i + 2]) & 0x3F);
    }
    if ((c >> 3) == 0x1E && i + 3 < s.size()) {
        len = 4;
        return (static_cast<uint32_t>(c & 0x07) << 18) |
               ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 12) |
               ((static_cast<unsigned char>(s[i + 2]) & 0x3F) << 6) |
               (static_cast<unsigned char>(s[i + 3]) & 0x3F);
    }
    len = 1;
    return c; // invalid lead byte: treat as Latin-1
}

// ── JsonLint messages ──────────────────────────────────────────────────────
static std::string ParamToString(const LintParam& p) {
    if (std::holds_alternative<int64_t>(p)) return std::to_string(std::get<int64_t>(p));
    if (std::holds_alternative<char>(p)) return std::string(1, std::get<char>(p));
    if (std::holds_alternative<std::string>(p)) return std::get<std::string>(p);
    return "";
}

static std::string Format1(const char* fmt, const LintParam& p1) {
    std::string out(fmt);
    std::string v = ParamToString(p1);
    size_t pos;
    while ((pos = out.find("{0}")) != std::string::npos) out.replace(pos, 3, v);
    return out;
}

static std::string Format2(const char* fmt, const LintParam& p1, const LintParam& p2) {
    std::string out = Format1(fmt, p1);
    std::string v = ParamToString(p2);
    size_t pos;
    while ((pos = out.find("{1}")) != std::string::npos) out.replace(pos, 3, v);
    return out;
}

std::string JsonLint::ParserStateName(ParserState s) {
    switch (s) {
        case ParserState::STRICT: return "STRICT";
        case ParserState::OK: return "OK";
        case ParserState::NAN_INF: return "NAN_INF";
        case ParserState::JSONC: return "JSONC";
        case ParserState::JSON5: return "JSON5";
        case ParserState::BAD: return "BAD";
        case ParserState::FATAL: return "FATAL";
        case ParserState::SCHEMA: return "SCHEMA";
    }
    return "?";
}

std::string JsonLint::CharDisplay(char c) {
    switch (c) {
        case '\x00': return "'\\x00'";
        case '\t': return "'\\t'";
        case '\r': return "'\\r'";
        case '\n': return "'\\n'";
        case '\'': return "'\\''";
        default: return std::string("'") + c + "'";
    }
}

std::string JsonLint::message() const {
    using T = JsonLintType;
    switch (lintType) {
    case T::OK_CONTROL_CHAR: return "Control characters (ASCII code less than 0x20) are disallowed inside strings under the strict JSON specification";
    case T::NAN_INF_Infinity: return "Infinity is not allowed in the original JSON specification";
    case T::NAN_INF_NaN: return "NaN is not allowed in the original JSON specification";
    case T::JSONC_JAVASCRIPT_COMMENT: return "JavaScript comments are not allowed in the original JSON specification";
    case T::JSON5_WHITESPACE_CHAR: return "Whitespace characters other than ' ', '\\t', '\\r', and '\\n' are only allowed in JSON5";
    case T::JSON5_SINGLEQUOTED_STRING: return "Singlequoted strings are only allowed in JSON5";
    case T::JSON5_ESCAPED_NEWLINE: return "Escaped newline characters are only allowed in JSON5";
    case T::JSON5_X_ESCAPE: return "\\x escapes are only allowed in JSON5";
    case T::JSON5_ESCAPED_CHAR: return Format1("Escaped char '{0}' is only allowed in JSON5", param1);
    case T::JSON5_UNQUOTED_KEY: return "Unquoted keys are only allowed in JSON5";
    case T::JSON5_NUM_LEADING_PLUS: return "Leading + signs in numbers are only allowed in JSON5";
    case T::JSON5_HEX_NUM: return "Hexadecimal numbers are only allowed in JSON5";
    case T::JSON5_NUM_LEADING_DECIMAL_POINT: return "Numbers with a leading decimal point are only allowed in JSON5";
    case T::JSON5_COMMA_AFTER_LAST_ELEMENT_ARRAY: return "Comma after last element of array";
    case T::JSON5_COMMA_AFTER_LAST_ELEMENT_OBJECT: return "Comma after last key-value pair of object";
    case T::JSON5_NUM_TRAILING_DECIMAL_POINT: return "Numbers with a trailing decimal point are only allowed in JSON5";
    case T::BAD_UNTERMINATED_MULTILINE_COMMENT: return "Unterminated multi-line comment";
    case T::BAD_PYTHON_COMMENT: return "Python-style '#' comments are not allowed in any well-accepted JSON specification";
    case T::BAD_STRING_CONTAINS_NEWLINE: return "String literal contains newline";
    case T::BAD_KEY_CONTAINS_NEWLINE: return "Object key contains newline";
    case T::BAD_UNTERMINATED_STRING: return Format1("Unterminated string literal starting at position {0}", param1);
    case T::BAD_INVALID_UNQUOTED_KEY: return Format1("No valid unquoted key beginning at {0}", param1);
    case T::BAD_PYTHON_nan: return "nan is not a valid representation of Not a Number in JSON";
    case T::BAD_PYTHON_None: return "None is not allowed in any JSON specification";
    case T::BAD_PYTHON_inf: return "inf is not the correct representation of Infinity in JSON";
    case T::BAD_UNNECESSARY_LEADING_0: return "Numbers with an unnecessary leading 0 (like \"01\") are not allowed in any JSON specification";
    case T::BAD_SLASH_FRACTION: return "Fractions of the form 1/3 are not allowed in any JSON specification";
    case T::BAD_NUMBER_INVALID_FORMAT: return Format1("Number string {0} had bad format", param1);
    case T::BAD_TWO_CONSECUTIVE_COMMAS_ARRAY: return Format1("Two consecutive commas after element {0} of array", param1);
    case T::BAD_COMMA_BEFORE_FIRST_ELEMENT_ARRAY: return "Comma before first value in array";
    case T::BAD_ARRAY_ENDSWITH_CURLYBRACE: return "Expected ']' at the end of an array, but found '}'";
    case T::BAD_NO_COMMA_BETWEEN_ARRAY_ITEMS: return "No comma between array members";
    case T::BAD_COLON_BETWEEN_ARRAY_ITEMS: return "':' (key-value separator) where ',' between array members expected. Maybe you forgot to close the array?";
    case T::BAD_UNTERMINATED_ARRAY: return "Unterminated array";
    case T::BAD_TWO_CONSECUTIVE_COMMAS_OBJECT: return Format1("Two consecutive commas after key-value pair {0} of object", param1);
    case T::BAD_COMMA_BEFORE_FIRST_PAIR_OBJECT: return "Comma before first value in object";
    case T::BAD_NO_COMMA_BETWEEN_OBJECT_PAIRS: return Format1("No comma after key-value pair {0} in object", param1);
    case T::BAD_UNTERMINATED_OBJECT: return "Unterminated object";
    case T::BAD_OBJECT_ENDSWITH_SQUAREBRACE: return "Expected '}' at the end of an object, but found ']'";
    case T::BAD_COLON_BETWEEN_OBJECT_PAIRS: return "':' found instead of comma after key-value pair";
    case T::BAD_CHAR_WHERE_COLON_EXPECTED: return Format2("Found '{0}' after key {1} when colon expected", param1, param2);
    case T::BAD_NO_COLON_BETWEEN_OBJECT_KEY_VALUE: return Format1("No ':' between key {0} and value {0} of object", param1);
    case T::BAD_DUPLICATE_KEY: return Format1("Object has multiple of key \"{0}\"", param1);
    case T::BAD_PYTHON_True: return "True is not allowed in any JSON specification";
    case T::BAD_PYTHON_False: return "False is not allowed in any JSON specification";
    case T::BAD_JAVASCRIPT_undefined: return "undefined is not allowed in any JSON specification";
    case T::BAD_CHAR_INSTEAD_OF_EOF: return Format1("At end of valid JSON document, got {0} instead of EOF", param1);
    case T::BAD_FLOAT_TOO_LARGE: return Format1("Number string {0} is too large for a 64-bit floating point number", param1);
    case T::FATAL_EXPECTED_JAVASCRIPT_COMMENT: return "Expected JavaScript comment after '/'";
    case T::FATAL_HEXADECIMAL_TOO_SHORT: return Format1("Could not find valid hexadecimal of length {0}", param1);
    case T::FATAL_UNTERMINATED_KEY: return "Unterminated object key";
    case T::FATAL_INVALID_STARTSWITH_n: return "Expected literal starting with 'n' to be null or nan";
    case T::FATAL_PLUS_OR_MINUS_AT_EOF: return Format1("'{0}' sign at end of document", param1);
    case T::FATAL_INVALID_STARTSWITH_I: return "Expected literal starting with 'I' to be Infinity";
    case T::FATAL_INVALID_STARTSWITH_N: return "Expected literal starting with 'N' to be NaN or None";
    case T::FATAL_INVALID_STARTSWITH_i: return "Expected literal starting with 'i' to be inf";
    case T::FATAL_HEX_INT_OVERFLOW: return "Hex number too large for a 64-bit signed integer type";
    case T::FATAL_SECOND_DECIMAL_POINT: return "Number with a decimal point in the wrong place";
    case T::FATAL_NUM_TRAILING_e_OR_E: return "Scientific notation 'e' with no number following";
    case T::FATAL_MAX_RECURSION_DEPTH: return "Maximum recursion depth (512) reached";
    case T::FATAL_UNEXPECTED_EOF: return "Unexpected end of file";
    case T::FATAL_NO_VALID_LITERAL_POSSIBLE: return "No valid literal possible";
    case T::FATAL_INVALID_STARTSWITH_t: return "Expected literal starting with 't' to be true";
    case T::FATAL_INVALID_STARTSWITH_f: return "Expected literal starting with 'f' to be false";
    case T::FATAL_INVALID_STARTSWITH_T: return "Expected literal starting with 'T' to be True";
    case T::FATAL_INVALID_STARTSWITH_F: return "Expected literal starting with 'F' to be False";
    case T::FATAL_INVALID_STARTSWITH_u: return "Expected literal starting with 'u' to be undefined";
    case T::FATAL_BADLY_LOCATED_CHAR: return Format1("Badly located character {0}", param1);
    case T::FATAL_NO_INPUT: return "No input";
    case T::FATAL_ONLY_WHITESPACE_COMMENTS: return "Input is only whitespace and maybe comments";
    case T::FATAL_JSONL_NOT_ONE_DOC_PER_LINE: return "JSON Lines document does not contain exactly one JSON document per line";
    default: // FATAL_UNSPECIFIED_ERROR and all SCHEMA_* wrap a prebuilt message
        return ParamToString(param1);
    }
}

std::string JsonLint::ToString() const {
    return "Syntax error (severity = " + ParserStateName(severity()) + ") at position " +
           std::to_string(pos) + " (char " + CharDisplay(curChar) + "): " + message();
}

JNodePtr JsonLint::ToJson() const {
    auto obj = std::make_shared<JObject>();
    obj->children.set("message", JNode::Str(message()));
    obj->children.set("position", JNode::Int(pos));
    obj->children.set("severity", JNode::Str(ParserStateName(severity())));
    return obj;
}

// ── UnescapedJsonString (declared on JNode, needs the parser) ──────────────
std::string JNode::UnescapedJsonString(const std::string& str, bool strAlreadyQuoted) {
    JsonParser parser;
    std::string quoted = strAlreadyQuoted ? str : "\"" + str + "\"";
    JNodePtr parsed = parser.ParseString(quoted);
    return parsed->IsStrValue() ? parsed->GetStr() : std::string();
}

// ── parser core ────────────────────────────────────────────────────────────
bool JsonParser::HandleError(JsonLintType lintType, const std::string& inp, int64_t pos,
                             LintParam param1, LintParam param2) {
    ParserState severity = static_cast<ParserState>(1 + (static_cast<int>(lintType) >> 10));
    if (state < severity) state = severity;
    bool isFatal = fatal();
    if (static_cast<int>(severity) > static_cast<int>(loggerLevel)) {
        char c = (pos >= static_cast<int64_t>(inp.size()) || pos < 0)
            ? '\x00' : inp[static_cast<size_t>(pos)];
        JsonLint newLint(lintType, utf8Pos(), c, std::move(param1), std::move(param2));
        lint.push_back(newLint);
        if (throwIfLogged || (isFatal && throwIfFatal)) throw JsonParserException(newLint, c);
    }
    return isFatal;
}

void JsonParser::ConsumeLine(const std::string& inp) {
    while (ii < static_cast<int64_t>(inp.size())) {
        char c = inp[static_cast<size_t>(ii++)];
        if (c == '\n') return;
    }
}

int64_t JsonParser::EndOfPreviousLine(const std::string& inp, int64_t ii, int64_t start) {
    int64_t len = static_cast<int64_t>(inp.size());
    int64_t pos = ii >= len ? len - 1 : ii - 1;
    if (pos <= start) {
        if (start == len - 1) return len;
        return start;
    }
    char c = inp[static_cast<size_t>(pos)];
    if (c != '\n') return pos + 1;
    if (pos >= 1 && inp[static_cast<size_t>(pos - 1)] == '\r') return pos - 1;
    return pos;
}

// JSON5-only whitespace code points (beyond ' ', '\t', '\r', '\n')
static bool IsJson5Whitespace(uint32_t cp) {
    switch (cp) {
        case 0x2028: case 0x2029: case 0xFEFF: case 0xA0: case 0x1680:
        case 0x2000: case 0x2001: case 0x2002: case 0x2003: case 0x2004:
        case 0x2005: case 0x2006: case 0x2007: case 0x2008: case 0x2009:
        case 0x200A: case 0x202F: case 0x205F: case 0x3000:
            return true;
        default: return false;
    }
}

bool JsonParser::ConsumeInsignificantChars(const std::string& inp) {
    int64_t len = static_cast<int64_t>(inp.size());
    while (ii < len) {
        char c = inp[static_cast<size_t>(ii)];
        switch (c) {
        case ' ': case '\t': case '\r': case '\n':
            ii++;
            break;
        case '/': {
            int64_t commentStartUtf8 = utf8Pos();
            int64_t commentContentStartII = ii + 2;
            int64_t commentContentEndII;
            bool isMultiline;
            ii++;
            if (ii == len) {
                HandleError(JsonLintType::FATAL_EXPECTED_JAVASCRIPT_COMMENT, inp, len - 1);
                return false;
            }
            HandleError(JsonLintType::JSONC_JAVASCRIPT_COMMENT, inp, ii);
            c = inp[static_cast<size_t>(ii)];
            if (c == '/') {
                isMultiline = false;
                ConsumeLine(inp);
                commentContentEndII = EndOfPreviousLine(inp, ii, commentContentStartII);
            } else if (c == '*') {
                isMultiline = true;
                bool commentEnded = false;
                while (ii < len - 1) {
                    c = inp[static_cast<size_t>(ii++)];
                    if (c == '*' && inp[static_cast<size_t>(ii)] == '/') {
                        commentEnded = true;
                        ii++;
                        break;
                    }
                }
                if (!commentEnded) {
                    HandleError(JsonLintType::BAD_UNTERMINATED_MULTILINE_COMMENT, inp, len - 1);
                    ii++;
                    return false;
                }
                commentContentEndII = ii - 2;
            } else {
                HandleError(JsonLintType::FATAL_EXPECTED_JAVASCRIPT_COMMENT, inp, ii);
                return false;
            }
            if (rememberComments && commentContentEndII >= commentContentStartII)
                comments.emplace_back(inp.substr(static_cast<size_t>(commentContentStartII),
                                                 static_cast<size_t>(commentContentEndII - commentContentStartII)),
                                      isMultiline, commentStartUtf8);
            break;
        }
        case '#': {
            // Python-style single-line comment
            int64_t commentStartUtf8 = utf8Pos();
            int64_t commentContentStartII = ii + 1;
            HandleError(JsonLintType::BAD_PYTHON_COMMENT, inp, ii);
            ConsumeLine(inp);
            int64_t commentContentEndII = EndOfPreviousLine(inp, ii, commentContentStartII);
            if (rememberComments && commentContentEndII >= commentContentStartII)
                comments.emplace_back(inp.substr(static_cast<size_t>(commentContentStartII),
                                                 static_cast<size_t>(commentContentEndII - commentContentStartII)),
                                      false, commentStartUtf8);
            break;
        }
        default: {
            unsigned char uc = static_cast<unsigned char>(c);
            if (uc >= 0x80) {
                size_t cpLen;
                uint32_t cp = DecodeCodePoint(inp, static_cast<size_t>(ii), cpLen);
                if (IsJson5Whitespace(cp)) {
                    HandleError(JsonLintType::JSON5_WHITESPACE_CHAR, inp, ii);
                    ii += static_cast<int64_t>(cpLen);
                    break;
                }
            }
            return true;
        }
        }
    }
    return true;
}

int JsonParser::ParseHexChar(const std::string& inp, int length) {
    int64_t len = static_cast<int64_t>(inp.size());
    if (ii >= len - length) {
        HandleError(JsonLintType::FATAL_HEXADECIMAL_TOO_SHORT, inp, ii, static_cast<int64_t>(length));
        return -1;
    }
    int64_t end = ii + length > len ? len : ii + length;
    int charval = 0;
    for (int64_t k = ii; k < end; k++) {
        char h = inp[static_cast<size_t>(k)];
        int digit;
        if (h >= '0' && h <= '9') digit = h - '0';
        else if (h >= 'a' && h <= 'f') digit = h - 'a' + 10;
        else if (h >= 'A' && h <= 'F') digit = h - 'A' + 10;
        else {
            ii = end - 1;
            HandleError(JsonLintType::FATAL_HEXADECIMAL_TOO_SHORT, inp, ii, static_cast<int64_t>(length));
            return -1;
        }
        charval = (charval << 4) | digit;
    }
    // the -1 is because ParseString increments by 1 after every escaped sequence
    ii = end - 1;
    return charval;
}

bool JsonParser::HandleCharErrors(int64_t c, const std::string& inp, int64_t pos) {
    if (c < 0x20) {
        if (c == '\n')
            return HandleError(JsonLintType::BAD_STRING_CONTAINS_NEWLINE, inp, pos);
        if (c < 0) return true;
        return HandleError(JsonLintType::OK_CONTROL_CHAR, inp, pos);
    }
    return false;
}

static bool EscapeMap(char c, char& out) {
    switch (c) {
        case '\\': out = '\\'; return true;
        case 'n': out = '\n'; return true;
        case 'r': out = '\r'; return true;
        case 'b': out = '\b'; return true;
        case 't': out = '\t'; return true;
        case 'f': out = '\f'; return true;
        case '/': out = '/'; return true;
        case 'v': out = '\x0b'; return true;
        case '\'': out = '\''; return true;
        case '"': out = '"'; return true;
        default: return false;
    }
}

void JsonParser::AppendCodeUnit(std::string& sb, int unit, int& pendingHighSurrogate) {
    if (pendingHighSurrogate != 0) {
        if (unit >= 0xDC00 && unit <= 0xDFFF) {
            uint32_t cp = 0x10000 +
                ((static_cast<uint32_t>(pendingHighSurrogate) - 0xD800) << 10) +
                (static_cast<uint32_t>(unit) - 0xDC00);
            pendingHighSurrogate = 0;
            AppendCodePoint(sb, cp);
            return;
        }
        // unpaired high surrogate: emit it WTF-8 style, then process `unit`
        AppendCodePoint(sb, static_cast<uint32_t>(pendingHighSurrogate));
        pendingHighSurrogate = 0;
    }
    if (unit >= 0xD800 && unit <= 0xDBFF) {
        pendingHighSurrogate = unit;
        return;
    }
    AppendCodePoint(sb, static_cast<uint32_t>(unit));
}

void JsonParser::FlushPendingSurrogate(std::string& sb, int& pendingHighSurrogate) {
    if (pendingHighSurrogate != 0) {
        AppendCodePoint(sb, static_cast<uint32_t>(pendingHighSurrogate));
        pendingHighSurrogate = 0;
    }
}

JNodePtr JsonParser::ParseString(const std::string& inp) {
    int64_t len = static_cast<int64_t>(inp.size());
    int64_t startUtf8Pos = ii;
    char quoteChar = inp[static_cast<size_t>(ii++)];
    if (quoteChar == '\'')
        HandleError(JsonLintType::JSON5_SINGLEQUOTED_STRING, inp, ii);
    std::string sb;
    int pendingHighSurrogate = 0;
    while (true) {
        if (ii >= len) {
            HandleError(JsonLintType::BAD_UNTERMINATED_STRING, inp, ii - 1, startUtf8Pos);
            break;
        }
        char c = inp[static_cast<size_t>(ii)];
        if (c == quoteChar) {
            break;
        } else if (c == '\\') {
            if (ii >= len - 2) {
                HandleError(JsonLintType::BAD_UNTERMINATED_STRING, inp, len - 1, startUtf8Pos);
                ii++;
                continue;
            }
            char nextChar = inp[static_cast<size_t>(ii + 1)];
            char escapedChar;
            if (nextChar == quoteChar) {
                FlushPendingSurrogate(sb, pendingHighSurrogate);
                sb += quoteChar;
                ii += 1;
            } else if (EscapeMap(nextChar, escapedChar)) {
                FlushPendingSurrogate(sb, pendingHighSurrogate);
                sb += escapedChar;
                ii += 1;
            } else if (nextChar == 'u') {
                ii += 2;
                int nextHex = ParseHexChar(inp, 4);
                if (HandleCharErrors(nextHex, inp, ii)) break;
                AppendCodeUnit(sb, nextHex, pendingHighSurrogate);
            } else if (nextChar == '\n' || nextChar == '\r') {
                HandleError(JsonLintType::JSON5_ESCAPED_NEWLINE, inp, ii + 1);
                ii++;
                if (nextChar == '\r' && ii < len - 1 && inp[static_cast<size_t>(ii + 1)] == '\n')
                    ii++;
            } else if (nextChar == 'x') {
                ii += 2;
                int nextHex = ParseHexChar(inp, 2);
                if (HandleCharErrors(nextHex, inp, ii)) break;
                HandleError(JsonLintType::JSON5_X_ESCAPE, inp, ii);
                FlushPendingSurrogate(sb, pendingHighSurrogate);
                AppendCodePoint(sb, static_cast<uint32_t>(nextHex));
            } else {
                HandleError(JsonLintType::JSON5_ESCAPED_CHAR, inp, ii + 1, nextChar);
            }
        } else {
            if (HandleCharErrors(static_cast<unsigned char>(c), inp, ii)) break;
            FlushPendingSurrogate(sb, pendingHighSurrogate);
            sb += c; // bytes >= 0x80 pass through, keeping multibyte chars intact
        }
        ii++;
    }
    ii++;
    FlushPendingSurrogate(sb, pendingHighSurrogate);
    return JNode::Str(std::move(sb), startUtf8Pos);
}

bool JsonParser::ParseKey(const std::string& inp, std::string& key) {
    int64_t len = static_cast<int64_t>(inp.size());
    char quoteChar = inp[static_cast<size_t>(ii)];
    if (quoteChar == '\'')
        HandleError(JsonLintType::JSON5_SINGLEQUOTED_STRING, inp, ii);
    if (quoteChar != '\'' && quoteChar != '"') return ParseUnquotedKey(inp, key);
    ii++;
    std::string sb;
    int pendingHighSurrogate = 0;
    while (true) {
        if (ii >= len) {
            HandleError(JsonLintType::FATAL_UNTERMINATED_KEY, inp, ii - 1);
            return false;
        }
        char c = inp[static_cast<size_t>(ii)];
        if (c == quoteChar) {
            break;
        } else if (c == '\\') {
            if (ii >= len - 2) {
                HandleError(JsonLintType::FATAL_UNTERMINATED_KEY, inp, len - 1);
                return false;
            }
            char nextChar = inp[static_cast<size_t>(ii + 1)];
            char escapedChar;
            if (nextChar == quoteChar) {
                FlushPendingSurrogate(sb, pendingHighSurrogate);
                sb += quoteChar;
                ii++;
            } else if (EscapeMap(nextChar, escapedChar)) {
                FlushPendingSurrogate(sb, pendingHighSurrogate);
                sb += escapedChar;
                ii++;
            } else if (nextChar == 'u') {
                ii += 2;
                int nextHex = ParseHexChar(inp, 4);
                if (HandleCharErrors(nextHex, inp, ii)) break;
                AppendCodeUnit(sb, nextHex, pendingHighSurrogate);
            } else if (nextChar == '\n' || nextChar == '\r') {
                HandleError(JsonLintType::JSON5_ESCAPED_NEWLINE, inp, ii + 1);
                ii++;
                if (nextChar == '\r' && ii < len - 1 && inp[static_cast<size_t>(ii + 1)] == '\n')
                    ii++;
            } else if (nextChar == 'x') {
                ii += 2;
                int nextHex = ParseHexChar(inp, 2);
                if (HandleCharErrors(nextHex, inp, ii)) break;
                HandleError(JsonLintType::JSON5_X_ESCAPE, inp, ii);
                FlushPendingSurrogate(sb, pendingHighSurrogate);
                AppendCodePoint(sb, static_cast<uint32_t>(nextHex));
            } else {
                HandleError(JsonLintType::JSON5_ESCAPED_CHAR, inp, ii + 1, nextChar);
            }
        } else if (static_cast<unsigned char>(c) < 0x20) {
            if (c == '\n')
                HandleError(JsonLintType::BAD_KEY_CONTAINS_NEWLINE, inp, ii);
            else
                HandleError(JsonLintType::OK_CONTROL_CHAR, inp, ii);
            FlushPendingSurrogate(sb, pendingHighSurrogate);
            sb += c;
        } else {
            FlushPendingSurrogate(sb, pendingHighSurrogate);
            sb += c;
        }
        ii++;
    }
    ii++;
    FlushPendingSurrogate(sb, pendingHighSurrogate);
    key = std::move(sb);
    return true;
}

// Same pattern as the C# UNQUOTED_KEY_REGEX (JSON5 IdentifierName, compiled
// with IgnoreCase). ICU understands the same \p{...} categories.
static const char* UNQUOTED_START =
    "(?:[_\\$\\p{Lu}\\p{Ll}\\p{Lt}\\p{Lm}\\p{Lo}\\p{Nl}]|\\\\u[\\da-f]{4})";

static Regex& UnquotedKeyRegex() {
    static Regex* re = new Regex(
        std::string(UNQUOTED_START) +
            "(?:[\\p{Mn}\\p{Mc}\\p{Nd}\\p{Pc}\\u200c\\u200d]|" + UNQUOTED_START + ")*",
        /*ignoreCase*/ true, /*multiline*/ false);
    return *re;
}

bool JsonParser::ParseUnquotedKey(const std::string& inp, std::string& key) {
    // Match against a bounded window to avoid re-encoding the whole document
    // per key (the C# regex also only ever matches at ii). 4096 bytes bounds
    // the key length, which no sane document exceeds.
    size_t start = static_cast<size_t>(ii);
    size_t windowEnd = std::min(inp.size(), start + 4096);
    while (windowEnd < inp.size() &&
           (static_cast<unsigned char>(inp[windowEnd]) & 0xC0) == 0x80)
        windowEnd++; // don't split a UTF-8 char
    std::string window = inp.substr(start, windowEnd - start);
    Regex::Match match = UnquotedKeyRegex().FirstMatch(window, 0);
    if (!match.success || match.start != 0) {
        HandleError(JsonLintType::BAD_INVALID_UNQUOTED_KEY, inp, ii, ii);
        return false;
    }
    HandleError(JsonLintType::JSON5_UNQUOTED_KEY, inp, ii);
    std::string result = match.value;
    ii += static_cast<int64_t>(result.size());
    // fix \uXXXX escapes inside the key (combining surrogate pairs)
    if (result.find("\\u") != std::string::npos) {
        std::string sb;
        int pendingHighSurrogate = 0;
        size_t i = 0;
        while (i < result.size()) {
            if (result[i] == '\\' && i + 5 < result.size() && result[i + 1] == 'u') {
                int unit = 0;
                bool ok = true;
                for (size_t k = i + 2; k < i + 6; k++) {
                    char h = result[k];
                    int digit;
                    if (h >= '0' && h <= '9') digit = h - '0';
                    else if (h >= 'a' && h <= 'f') digit = h - 'a' + 10;
                    else if (h >= 'A' && h <= 'F') digit = h - 'A' + 10;
                    else { ok = false; break; }
                    unit = (unit << 4) | digit;
                }
                if (ok) {
                    if (HandleCharErrors(unit, inp, ii)) return false;
                    AppendCodeUnit(sb, unit, pendingHighSurrogate);
                    i += 6;
                    continue;
                }
            }
            FlushPendingSurrogate(sb, pendingHighSurrogate);
            sb += result[i++];
        }
        FlushPendingSurrogate(sb, pendingHighSurrogate);
        result = std::move(sb);
    }
    key = std::move(result);
    return true;
}

// parse an int64 like C# long.Parse: optional sign, then only digits.
// outcome: 0 = ok, 1 = format error, 2 = overflow
static int TryParseInt64(const std::string& numstr, int64_t& out) {
    if (numstr.empty()) return 1;
    size_t i = 0;
    bool neg = false;
    if (numstr[0] == '-' || numstr[0] == '+') {
        neg = numstr[0] == '-';
        i = 1;
    }
    if (i >= numstr.size()) return 1;
    uint64_t acc = 0;
    for (; i < numstr.size(); i++) {
        char c = numstr[i];
        if (c < '0' || c > '9') return 1;
        uint64_t digit = static_cast<uint64_t>(c - '0');
        if (acc > (UINT64_MAX - digit) / 10) return 2;
        acc = acc * 10 + digit;
    }
    if (neg) {
        if (acc > static_cast<uint64_t>(INT64_MAX) + 1) return 2;
        out = acc == static_cast<uint64_t>(INT64_MAX) + 1
            ? INT64_MIN : -static_cast<int64_t>(acc);
    } else {
        if (acc > static_cast<uint64_t>(INT64_MAX)) return 2;
        out = static_cast<int64_t>(acc);
    }
    return 0;
}

// parse a double like C# double.Parse(s, dot-decimal): optional sign, digits,
// '.', exponent. outcome: 0 = ok, 1 = format error, 2 = overflow.
// strtod_l with a fixed C locale: the user's locale may use ',' as the
// decimal separator (upstream issue #17), but JSON is always '.'
static int TryParseDouble(const std::string& numstr, double& out) {
    static locale_t cLocale = newlocale(LC_ALL_MASK, "C", nullptr);
    if (numstr.empty()) return 1;
    errno = 0;
    const char* cstr = numstr.c_str();
    char* endp = nullptr;
    // reject strtod-isms C# does not accept: hex floats, inf/nan words
    for (char ch : numstr) {
        if (!((ch >= '0' && ch <= '9') || ch == '.' || ch == '+' || ch == '-' ||
              ch == 'e' || ch == 'E'))
            return 1;
    }
    double d = strtod_l(cstr, &endp, cLocale);
    if (endp != cstr + numstr.size() || endp == cstr) return 1;
    if (errno == ERANGE && (d == HUGE_VAL || d == -HUGE_VAL)) return 2;
    out = d;
    return 0;
}

JNodePtr JsonParser::ParseNumber(const std::string& inp) {
    int64_t len = static_cast<int64_t>(inp.size());
    // `parsed` tracks which portions of a number have been parsed:
    // int part = 1, +decimal point = 3, +scientific notation = 7
    int parsed = 1;
    int64_t start = ii;
    int64_t startUtf8Pos = start;
    char c = inp[static_cast<size_t>(ii)];
    bool negative = false;
    if (c < '0' || c > '9') {
        if (c == 'n') {
            if (ii <= len - 4 && inp.compare(static_cast<size_t>(ii), 4, "null") == 0) {
                ii += 4;
                return JNode::Null(startUtf8Pos);
            }
            if (ii <= len - 3 && inp.compare(static_cast<size_t>(ii), 3, "nan") == 0) {
                HandleError(JsonLintType::BAD_PYTHON_nan, inp, ii);
                ii += 3;
                return JNode::Float(kNan, startUtf8Pos);
            }
            HandleError(JsonLintType::FATAL_INVALID_STARTSWITH_n, inp, ii + 1);
            return JNode::Null(startUtf8Pos);
        }
        if (c == '-' || c == '+') {
            if (c == '+')
                HandleError(JsonLintType::JSON5_NUM_LEADING_PLUS, inp, ii);
            else
                negative = true;
            ii++;
            if (ii >= len) {
                HandleError(JsonLintType::FATAL_PLUS_OR_MINUS_AT_EOF, inp, len - 1, c);
                return JNode::Null(startUtf8Pos);
            }
        }
        c = inp[static_cast<size_t>(ii)];
        if (c == 'I') {
            if (ii <= len - 8 && inp.compare(static_cast<size_t>(ii), 8, "Infinity") == 0) {
                HandleError(JsonLintType::NAN_INF_Infinity, inp, ii);
                ii += 8;
                return JNode::Float(negative ? -kInf : kInf, startUtf8Pos);
            }
            HandleError(JsonLintType::FATAL_INVALID_STARTSWITH_I, inp, ii + 1);
            return JNode::Null(startUtf8Pos);
        } else if (c == 'N') {
            if (ii <= len - 3 && inp.compare(static_cast<size_t>(ii), 3, "NaN") == 0) {
                HandleError(JsonLintType::NAN_INF_NaN, inp, ii);
                ii += 3;
                return JNode::Float(kNan, startUtf8Pos);
            }
            if (ii <= len - 4 && inp.compare(static_cast<size_t>(ii), 4, "None") == 0) {
                ii += 4;
                HandleError(JsonLintType::BAD_PYTHON_None, inp, ii);
                return JNode::Null(startUtf8Pos);
            }
            HandleError(JsonLintType::FATAL_INVALID_STARTSWITH_N, inp, ii + 1);
            return JNode::Null(startUtf8Pos);
        } else if (c == 'i') {
            if (ii <= len - 3 && inp.compare(static_cast<size_t>(ii), 3, "inf") == 0) {
                HandleError(JsonLintType::BAD_PYTHON_inf, inp, ii);
                ii += 3;
                return JNode::Float(negative ? -kInf : kInf, startUtf8Pos);
            }
            HandleError(JsonLintType::FATAL_INVALID_STARTSWITH_i, inp, ii);
            return JNode::Null(startUtf8Pos);
        }
    }
    if (c == '0' && ii < len - 1) {
        char nextChar = inp[static_cast<size_t>(ii + 1)];
        if (nextChar == 'x') {
            HandleError(JsonLintType::JSON5_HEX_NUM, inp, ii);
            ii += 2;
            start = ii;
            while (ii < len) {
                c = inp[static_cast<size_t>(ii)];
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
                    break;
                ii++;
            }
            std::string hexstr = inp.substr(static_cast<size_t>(start), static_cast<size_t>(ii - start));
            errno = 0;
            char* endp = nullptr;
            long long hexnum = strtoll(hexstr.c_str(), &endp, 16);
            if (hexstr.empty() || endp != hexstr.c_str() + hexstr.size() || errno == ERANGE) {
                HandleError(JsonLintType::FATAL_HEX_INT_OVERFLOW, inp, start);
                return JNode::Null(startUtf8Pos);
            }
            return JNode::Int(negative ? -hexnum : hexnum, startUtf8Pos);
        } else if (nextChar >= '0' && nextChar <= '9') {
            HandleError(JsonLintType::BAD_UNNECESSARY_LEADING_0, inp, ii);
        }
    }
    while (ii < len) {
        c = inp[static_cast<size_t>(ii)];
        if (c >= '0' && c <= '9') {
            ii++;
        } else if (c == '.') {
            if (parsed != 1) {
                HandleError(JsonLintType::FATAL_SECOND_DECIMAL_POINT, inp, ii);
                break;
            }
            if (ii == start)
                HandleError(JsonLintType::JSON5_NUM_LEADING_DECIMAL_POINT, inp, startUtf8Pos);
            parsed = 3;
            ii++;
        } else if (c == 'e' || c == 'E') {
            if ((parsed & 4) != 0) break;
            if (ii >= 1 && inp[static_cast<size_t>(ii - 1)] == '.')
                HandleError(JsonLintType::JSON5_NUM_TRAILING_DECIMAL_POINT, inp, startUtf8Pos);
            parsed += 4;
            ii++;
            if (ii < len) {
                c = inp[static_cast<size_t>(ii)];
                if (c == '+' || c == '-') ii++;
            } else {
                HandleError(JsonLintType::FATAL_NUM_TRAILING_e_OR_E, inp, len - 1);
                return JNode::Null(startUtf8Pos);
            }
        } else if (c == '/' && ii < len - 1) {
            char nextC = inp[static_cast<size_t>(ii + 1)];
            if (!((nextC >= '0' && nextC <= '9') || nextC == '-' || nextC == '.' || nextC == '+'))
                break;
            HandleError(JsonLintType::BAD_SLASH_FRACTION, inp, startUtf8Pos);
            double numer = 0;
            TryParseDouble(inp.substr(static_cast<size_t>(start), static_cast<size_t>(ii - start)), numer);
            ii++;
            JNodePtr denomNode = ParseNumber(inp);
            if (fatal()) return JNode::Float(numer, startUtf8Pos);
            double denom = denomNode->ToDouble();
            return JNode::Float(numer / denom, startUtf8Pos);
        } else {
            break;
        }
    }
    std::string numstr = inp.substr(static_cast<size_t>(start), static_cast<size_t>(ii - start));
    if (parsed == 1) {
        int64_t l;
        int res = TryParseInt64(numstr, l);
        if (res == 0) return JNode::Int(l, startUtf8Pos);
        if (res == 1) {
            HandleError(JsonLintType::BAD_NUMBER_INVALID_FORMAT, inp, startUtf8Pos,
                        JNode::StrToString(numstr, true));
            return JNode::Float(kNan, startUtf8Pos);
        }
        // overflow is OK: doubles represent much larger numbers, with loss of precision
    }
    double num;
    int res = TryParseDouble(numstr, num);
    if (res != 0) {
        JsonLintType errno_ = JsonLintType::BAD_NUMBER_INVALID_FORMAT;
        if (res == 2) {
            num = negative ? -kInf : kInf;
            errno_ = JsonLintType::BAD_FLOAT_TOO_LARGE;
        } else {
            num = kNan;
        }
        HandleError(errno_, inp, startUtf8Pos, JNode::StrToString(numstr, true));
    }
    if (!numstr.empty() && numstr.back() == '.')
        HandleError(JsonLintType::JSON5_NUM_TRAILING_DECIMAL_POINT, inp, startUtf8Pos);
    return JNode::Float(num, startUtf8Pos);
}

std::shared_ptr<JArray> JsonParser::ParseArray(const std::string& inp, int recursionDepth) {
    int64_t len = static_cast<int64_t>(inp.size());
    auto arr = std::make_shared<JArray>(ii);
    bool alreadySeenComma = false;
    ii++;
    if (recursionDepth == MAX_RECURSION_DEPTH) {
        HandleError(JsonLintType::FATAL_MAX_RECURSION_DEPTH, inp, ii);
        return arr;
    }
    while (ii < len) {
        if (!ConsumeInsignificantChars(inp)) return arr;
        if (ii >= len) break;
        char curC = inp[static_cast<size_t>(ii)];
        if (curC == ',') {
            if (alreadySeenComma)
                HandleError(JsonLintType::BAD_TWO_CONSECUTIVE_COMMAS_ARRAY, inp, ii,
                            static_cast<int64_t>(arr->children.size()) - 1);
            alreadySeenComma = true;
            if (arr->children.empty())
                HandleError(JsonLintType::BAD_COMMA_BEFORE_FIRST_ELEMENT_ARRAY, inp, ii);
            ii++;
            continue;
        } else if (curC == ']') {
            if (alreadySeenComma)
                HandleError(JsonLintType::JSON5_COMMA_AFTER_LAST_ELEMENT_ARRAY, inp, ii);
            ii++;
            return arr;
        } else if (curC == '}') {
            HandleError(JsonLintType::BAD_ARRAY_ENDSWITH_CURLYBRACE, inp, ii);
            if (alreadySeenComma)
                HandleError(JsonLintType::JSON5_COMMA_AFTER_LAST_ELEMENT_ARRAY, inp, ii);
            ii++;
            return arr;
        } else {
            if (!arr->children.empty() && !alreadySeenComma)
                HandleError(JsonLintType::BAD_NO_COMMA_BETWEEN_ARRAY_ITEMS, inp, ii);
            alreadySeenComma = false;
            int64_t iiBeforeParse = ii;
            JNodePtr newObj = ParseSomething(inp, recursionDepth);
            if (newObj->type == DT_STR && ii < len && inp[static_cast<size_t>(ii)] == ':') {
                // maybe the user forgot the closing ']' of an array that's the child of an object
                HandleError(JsonLintType::BAD_COLON_BETWEEN_ARRAY_ITEMS, inp, ii);
                ii = iiBeforeParse;
                return arr;
            }
            arr->children.push_back(newObj);
            if (fatal()) return arr;
        }
    }
    ii++;
    HandleError(JsonLintType::BAD_UNTERMINATED_ARRAY, inp, len - 1);
    return arr;
}

std::shared_ptr<JObject> JsonParser::ParseObject(const std::string& inp, int recursionDepth) {
    int64_t len = static_cast<int64_t>(inp.size());
    auto obj = std::make_shared<JObject>(ii);
    bool alreadySeenComma = false;
    ii++;
    if (recursionDepth == MAX_RECURSION_DEPTH) {
        HandleError(JsonLintType::FATAL_MAX_RECURSION_DEPTH, inp, ii);
        return obj;
    }
    while (ii < len) {
        if (!ConsumeInsignificantChars(inp)) return obj;
        if (ii >= len) break;
        char curC = inp[static_cast<size_t>(ii)];
        if (curC == ',') {
            if (alreadySeenComma)
                HandleError(JsonLintType::BAD_TWO_CONSECUTIVE_COMMAS_OBJECT, inp, ii,
                            static_cast<int64_t>(obj->children.size()) - 1);
            alreadySeenComma = true;
            if (obj->children.empty())
                HandleError(JsonLintType::BAD_COMMA_BEFORE_FIRST_PAIR_OBJECT, inp, ii);
            ii++;
            continue;
        } else if (curC == '}') {
            if (alreadySeenComma)
                HandleError(JsonLintType::JSON5_COMMA_AFTER_LAST_ELEMENT_OBJECT, inp, ii);
            ii++;
            return obj;
        } else if (curC == ']') {
            HandleError(JsonLintType::BAD_OBJECT_ENDSWITH_SQUAREBRACE, inp, ii);
            if (alreadySeenComma)
                HandleError(JsonLintType::JSON5_COMMA_AFTER_LAST_ELEMENT_OBJECT, inp, ii);
            ii++;
            return obj;
        } else { // expecting a key
            int64_t childCount = static_cast<int64_t>(obj->children.size());
            if (childCount > 0 && !alreadySeenComma) {
                HandleError(JsonLintType::BAD_NO_COMMA_BETWEEN_OBJECT_PAIRS, inp, ii, childCount - 1);
                if (ii < len - 1 && curC == ':') {
                    HandleError(JsonLintType::BAD_COLON_BETWEEN_OBJECT_PAIRS, inp, ii);
                    ii++;
                    ConsumeInsignificantChars(inp);
                    if (ii >= len) break;
                }
            }
            int64_t iiBeforeKey = ii;
            std::string key;
            if (!ParseKey(inp, key) || fatal()) {
                // key could be invalid if there's a valid JSON there that is not a valid key
                return obj;
            }
            if (ii >= len) break;
            if (inp[static_cast<size_t>(ii)] == ':') {
                ii++;
            } else {
                if (!ConsumeInsignificantChars(inp)) return obj;
                if (ii >= len) break;
                char c = inp[static_cast<size_t>(ii)];
                if (c == ':') {
                    ii++;
                } else if (c == ',' || c == ']') {
                    // might be a value in a parent array; bail out and reinterpret
                    HandleError(JsonLintType::BAD_CHAR_WHERE_COLON_EXPECTED, inp, ii, c, childCount);
                    ii = iiBeforeKey;
                    return obj;
                } else {
                    HandleError(JsonLintType::BAD_NO_COLON_BETWEEN_OBJECT_KEY_VALUE, inp, ii, childCount);
                }
            }
            if (!ConsumeInsignificantChars(inp)) return obj;
            if (ii >= len) break;
            JNodePtr val = ParseSomething(inp, recursionDepth);
            obj->children.set(key, val);
            if (fatal()) return obj;
            if (static_cast<int64_t>(obj->children.size()) == childCount)
                HandleError(JsonLintType::BAD_DUPLICATE_KEY, inp, ii, key);
            alreadySeenComma = false;
        }
    }
    ii++;
    HandleError(JsonLintType::BAD_UNTERMINATED_OBJECT, inp, len - 1);
    return obj;
}

JNodePtr JsonParser::ParseSomething(const std::string& inp, int recursionDepth) {
    int64_t len = static_cast<int64_t>(inp.size());
    int64_t startUtf8Pos = ii;
    if (ii >= len) {
        HandleError(JsonLintType::FATAL_UNEXPECTED_EOF, inp, len - 1);
        return JNode::Null(startUtf8Pos);
    }
    char curC = inp[static_cast<size_t>(ii)];
    if (curC == '"' || curC == '\'') return ParseString(inp);
    if ((curC >= '0' && curC <= '9') || curC == '-' || curC == '+' ||
        curC == 'n' || curC == 'I' || curC == 'N' || curC == '.' || curC == 'i')
        return ParseNumber(inp);
    if (curC == '[') return ParseArray(inp, recursionDepth + 1);
    if (curC == '{') return ParseObject(inp, recursionDepth + 1);
    if (ii > len - 4) {
        HandleError(JsonLintType::FATAL_NO_VALID_LITERAL_POSSIBLE, inp, ii);
        return JNode::Null(startUtf8Pos);
    }
    char nextC = inp[static_cast<size_t>(ii + 1)];
    if (curC == 't') {
        if (nextC == 'r' && inp[static_cast<size_t>(ii + 2)] == 'u' && inp[static_cast<size_t>(ii + 3)] == 'e') {
            ii += 4;
            return JNode::Bool(true, startUtf8Pos);
        }
        HandleError(JsonLintType::FATAL_INVALID_STARTSWITH_t, inp, ii + 1);
        return JNode::Null(startUtf8Pos);
    }
    if (curC == 'f') {
        if (ii <= len - 5 && nextC == 'a' && inp.compare(static_cast<size_t>(ii + 2), 3, "lse") == 0) {
            ii += 5;
            return JNode::Bool(false, startUtf8Pos);
        }
        HandleError(JsonLintType::FATAL_INVALID_STARTSWITH_f, inp, ii + 1);
        return JNode::Null(startUtf8Pos);
    }
    if (curC == 'T') {
        if (nextC == 'r' && inp[static_cast<size_t>(ii + 2)] == 'u' && inp[static_cast<size_t>(ii + 3)] == 'e') {
            ii += 4;
            HandleError(JsonLintType::BAD_PYTHON_True, inp, ii);
            return JNode::Bool(true, startUtf8Pos);
        }
        HandleError(JsonLintType::FATAL_INVALID_STARTSWITH_T, inp, ii + 1);
        return JNode::Null(startUtf8Pos);
    }
    if (curC == 'F') {
        if (ii <= len - 5 && nextC == 'a' && inp.compare(static_cast<size_t>(ii + 2), 3, "lse") == 0) {
            ii += 5;
            HandleError(JsonLintType::BAD_PYTHON_False, inp, ii);
            return JNode::Bool(false, startUtf8Pos);
        }
        HandleError(JsonLintType::FATAL_INVALID_STARTSWITH_F, inp, ii + 1);
        return JNode::Null(startUtf8Pos);
    }
    if (curC == 'u') {
        if (ii <= len - 9 && nextC == 'n' && inp.compare(static_cast<size_t>(ii + 2), 7, "defined") == 0) {
            ii += 9;
            HandleError(JsonLintType::BAD_JAVASCRIPT_undefined, inp, startUtf8Pos);
        } else {
            HandleError(JsonLintType::FATAL_INVALID_STARTSWITH_u, inp, ii + 1);
        }
        return JNode::Null(startUtf8Pos);
    }
    HandleError(JsonLintType::FATAL_BADLY_LOCATED_CHAR, inp, ii,
                ii >= len ? std::string("\"\\x00\"")
                          : JNode::StrToString(inp.substr(static_cast<size_t>(ii), 1), true));
    return JNode::Null(startUtf8Pos);
}

JNodePtr JsonParser::Parse(const std::string& inp) {
    Reset();
    if (inp.empty()) {
        HandleError(JsonLintType::FATAL_NO_INPUT, inp, 0);
        return std::make_shared<JNode>();
    }
    if (!ConsumeInsignificantChars(inp)) return std::make_shared<JNode>();
    if (ii >= static_cast<int64_t>(inp.size())) {
        HandleError(JsonLintType::FATAL_ONLY_WHITESPACE_COMMENTS, inp,
                    static_cast<int64_t>(inp.size()) - 1);
        return std::make_shared<JNode>();
    }
    JNodePtr json = ParseSomething(inp, 0);
    if (fatal()) return json;
    if (!ConsumeInsignificantChars(inp)) return json;
    if (ii < static_cast<int64_t>(inp.size()))
        HandleError(JsonLintType::BAD_CHAR_INSTEAD_OF_EOF, inp, ii, inp[static_cast<size_t>(ii)]);
    return json;
}

JNodePtr JsonParser::ParseJsonLines(const std::string& inp) {
    Reset();
    int64_t len = static_cast<int64_t>(inp.size());
    if (inp.empty()) {
        HandleError(JsonLintType::FATAL_NO_INPUT, inp, 0);
        return std::make_shared<JNode>();
    }
    if (!ConsumeInsignificantChars(inp)) return std::make_shared<JNode>();
    if (ii >= len) {
        HandleError(JsonLintType::FATAL_ONLY_WHITESPACE_COMMENTS, inp, len - 1);
        return std::make_shared<JNode>();
    }
    int64_t lastII = 0;
    auto arr = std::make_shared<JArray>(0);
    int64_t lineNum = 0;
    while (ii < len) {
        JNodePtr json = ParseSomething(inp, 0);
        int64_t endOfLastJson = ii;
        ConsumeInsignificantChars(inp);
        arr->children.push_back(json);
        if (fatal()) return arr;
        int64_t maxLastII = ii > len ? len : ii;
        for (; lastII < maxLastII; lastII++) {
            if (inp[static_cast<size_t>(lastII)] == '\n') {
                if (lastII < endOfLastJson) {
                    // e.g. "[1,\n2]" is invalid JSON Lines, but "[1,2]\n" is OK
                    HandleError(JsonLintType::FATAL_JSONL_NOT_ONE_DOC_PER_LINE, inp, lastII);
                    return arr;
                }
                lineNum++;
            }
        }
        // make sure this document was all on one line
        if (!(lineNum == static_cast<int64_t>(arr->Length()) ||
              (ii >= len && lineNum == static_cast<int64_t>(arr->Length()) - 1))) {
            if (ii >= len) ii = len - 1;
            HandleError(JsonLintType::FATAL_JSONL_NOT_ONE_DOC_PER_LINE, inp, ii);
            return arr;
        }
        if (!ConsumeInsignificantChars(inp)) return arr;
    }
    return arr;
}

void JsonParser::Reset() {
    lint.clear();
    comments.clear();
    state = ParserState::STRICT;
    ii = 0;
}

JNodePtr JsonParser::TryParseNumber(const std::string& inp, int64_t start, int64_t end,
                                    int64_t jnodePosition) {
    int64_t len = static_cast<int64_t>(inp.size());
    end = len < end ? len : end;
    if (start >= end) return JNode::Str("");
    int parsed = 1;
    int64_t ogStart = start;
    char c = inp[static_cast<size_t>(start)];
    bool negative = false;
    if (c < '0' || c > '9') {
        if (c == '-' || c == '+') {
            negative = c == '-';
            start++;
            if (start >= end)
                return JNode::Str(SubstringUnlessAll(inp, start, end), jnodePosition);
            c = inp[static_cast<size_t>(start)];
        }
        if (start == end - 8 && c == 'I' && inp.compare(static_cast<size_t>(start), 8, "Infinity") == 0)
            return JNode::Float(negative ? -kInf : kInf, jnodePosition);
        if (start == end - 3 && c == 'N' && inp.compare(static_cast<size_t>(start), 3, "NaN") == 0)
            return JNode::Float(kNan, jnodePosition);
    }
    int64_t ii = start;
    if (c == '0' && ii < end - 1 && inp[static_cast<size_t>(ii + 1)] == 'x') {
        ii += 2;
        start = ii;
        while (ii < end) {
            c = inp[static_cast<size_t>(ii)];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
                return JNode::Str(SubstringUnlessAll(inp, ogStart, end), jnodePosition);
            ii++;
        }
        std::string hexstr = inp.substr(static_cast<size_t>(start), static_cast<size_t>(end - start));
        errno = 0;
        char* endp = nullptr;
        long long hexnum = strtoll(hexstr.c_str(), &endp, 16);
        if (hexstr.empty() || endp != hexstr.c_str() + hexstr.size() || errno == ERANGE)
            return JNode::Str(SubstringUnlessAll(inp, ogStart, end), jnodePosition);
        return JNode::Int(negative ? -hexnum : hexnum, jnodePosition);
    }
    std::string numstr = SubstringUnlessAll(inp, ogStart, end);
    while (ii < end) {
        c = inp[static_cast<size_t>(ii)];
        if (c >= '0' && c <= '9') {
            ii++;
        } else if (c == '.') {
            if (parsed != 1) return JNode::Str(numstr, jnodePosition);
            parsed = 3;
            ii++;
        } else if (c == 'e' || c == 'E') {
            if ((parsed & 4) != 0) break;
            parsed += 4;
            ii++;
            if (ii < end) {
                c = inp[static_cast<size_t>(ii)];
                if (c == '+' || c == '-') ii++;
            } else {
                return JNode::Str(numstr, jnodePosition);
            }
        } else {
            return JNode::Str(numstr, jnodePosition);
        }
    }
    if (parsed == 1) {
        int64_t l;
        int res = TryParseInt64(numstr, l);
        if (res == 0) return JNode::Int(l, jnodePosition);
        // overflow falls through to double
    }
    double d;
    if (TryParseDouble(numstr, d) == 0) return JNode::Float(d, jnodePosition);
    return JNode::Str(numstr);
}

} // namespace jsontools
