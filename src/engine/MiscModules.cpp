// MiscModules.cpp — see MiscModules.h.

#include "MiscModules.h"

#include <cmath>
#include <cstdlib>
#include <limits>

#include "RemesPath.h" // ArgFunction::CsvRowRegex pieces, Regex

namespace jsontools {

// C# obj.ToString() semantics for YAML scalars: bools are "True"/"False",
// ints plain, strings themselves
static std::string CSharpValueToString(const JNodePtr& v) {
    switch (v->type) {
        case DT_BOOL: return v->GetBool() ? "True" : "False";
        case DT_INT: return std::to_string(v->GetInt());
        case DT_STR: return v->GetStr();
        default: return v->ToString();
    }
}

static bool ParsesAsDouble(const std::string& s) {
    if (s.empty()) return false;
    errno = 0;
    char* endp = nullptr;
    strtod(s.c_str(), &endp);
    return endp == s.c_str() + s.size();
}

// ── YamlDumper ──────────────────────────────────────────────────────────────
static std::string YamlEscapeBackslash(const std::string& s, bool isKey) {
    std::string sb = "\"";
    for (char c : s) {
        switch (c) {
            case '\x00': sb += "\\0"; break;
            case '\n': sb += "\\n"; break;
            case '\t': sb += "\\\\t"; break;
            case '\\': sb += "\\\\"; break;
            case '"': sb += "\\\""; break;
            case '\'': sb += isKey ? "''" : "\\'"; break;
            case '\f': sb += "\\\\f"; break;
            case '\b': sb += "\\\\b"; break;
            default: sb += c; break;
        }
    }
    sb += '"';
    return sb;
}

std::string YamlDumper::YamlKeyRepr(const std::string& k) {
    if (ParsesAsDouble(k)) return "'" + k + "'"; // enquote number-like keys
    for (char c : k) {
        if (c == '\t' || c == ' ' || c == ':' || c == '\x00') return YamlEscapeBackslash(k, true);
    }
    return k;
}

std::string YamlDumper::YamlValRepr(const JNodePtr& v) {
    if (v->type == DT_NULL) return "null";
    std::string strv = CSharpValueToString(v);
    if (v->type == DT_STR) {
        if (ParsesAsDouble(strv)) return "'" + strv + "'";
        if (!strv.empty() && (strv.front() == ' ' || strv.back() == ' '))
            return "\"" + strv + "\"";
        for (char c : strv) {
            if (c == '\\' || c == ':' || c == '"' || c == '\'' || c == '\r' || c == '\t' ||
                c == '\n' || c == '\f' || c == '\b' || c == '\x00')
                return YamlEscapeBackslash(strv, false);
        }
        return strv;
    }
    if (v->type == DT_FLOAT) {
        double d = v->GetDouble();
        if (d == std::numeric_limits<double>::infinity()) return ".inf";
        if (d == -std::numeric_limits<double>::infinity()) return "-.inf";
        if (std::isnan(d)) return ".nan";
        return JNode::DoubleToString(d);
    }
    return strv;
}

void YamlDumper::BuildYaml(std::string& sb, const JNodePtr& tok, int depth, int indent) {
    std::string indentSpace(static_cast<size_t>(indent * depth), ' ');
    if (JObject* o = AsObj(tok)) {
        if (o->Length() == 0) {
            sb += indentSpace + "{}\n";
            return;
        }
        for (auto& kv : o->children) {
            if ((kv.second->type & DT_ARR_OR_OBJ) != 0) {
                sb += indentSpace + YamlKeyRepr(kv.first) + ":\n";
                BuildYaml(sb, kv.second, depth + 1, indent);
            } else {
                sb += indentSpace + YamlKeyRepr(kv.first) + ": " + YamlValRepr(kv.second) + "\n";
            }
        }
    } else if (JArray* a = AsArr(tok)) {
        if (a->Length() == 0) {
            sb += indentSpace + "[]\n";
            return;
        }
        for (JNodePtr& child : a->children) {
            if ((child->type & DT_ARR_OR_OBJ) != 0) {
                sb += indentSpace + "-\n";
                BuildYaml(sb, child, depth + 1, indent);
            } else {
                sb += indentSpace + "- " + YamlValRepr(child) + "\n";
            }
        }
    } else {
        sb += YamlValRepr(tok) + "\n";
    }
}

std::string YamlDumper::Dump(const JNodePtr& json, int indent) {
    std::string sb;
    BuildYaml(sb, json, 0, indent);
    return sb;
}

// ── Dson ────────────────────────────────────────────────────────────────────
static std::string DsonFormatInteger(int64_t val) {
    if (val == INT64_MIN) return "1000000000000000000000";
    char buf[32];
    if (val < 0) {
        snprintf(buf, sizeof buf, "-%llo", static_cast<unsigned long long>(-val));
    } else {
        snprintf(buf, sizeof buf, "%llo", static_cast<unsigned long long>(val));
    }
    return buf;
}

std::string Dson::Dump(const JNodePtr& json) {
    switch (json->type) {
        case DT_ARR: {
            std::string sb = "so ";
            JArray* arr = AsArr(json);
            bool useAndDelim = true;
            for (size_t ii = 0; ii < arr->Length(); ii++) {
                sb += Dump(arr->children[ii]);
                sb += useAndDelim ? " and " : " also ";
                useAndDelim = !useAndDelim;
            }
            if (arr->Length() > 0) {
                size_t lengthToRemove = useAndDelim ? 5 : 4;
                sb.erase(sb.size() - lengthToRemove - 1, lengthToRemove);
            }
            sb += "many";
            return sb;
        }
        case DT_OBJ: {
            static const char* kvDelims = ",.!?";
            std::string sb = "such ";
            int delimIdx = 0;
            JObject* obj = AsObj(json);
            for (auto& kv : obj->children) {
                sb += JNode::StrToString(kv.first, true) + " is ";
                sb += Dump(kv.second);
                sb += kvDelims[delimIdx % 4];
                sb += ' ';
                delimIdx++;
            }
            if (obj->Length() > 0) sb.erase(sb.size() - 2, 1);
            sb += "wow";
            return sb;
        }
        case DT_BOOL:
            return json->GetBool() ? "yes" : "no";
        case DT_NULL:
            return "empty";
        case DT_INT:
            return DsonFormatInteger(json->GetInt());
        case DT_FLOAT: {
            double val = json->GetDouble();
            if (std::isinf(val) || std::isnan(val))
                throw DsonDumpException(JNode::DoubleToString(val) +
                                        " is fake number, can't understand. So silly, wow");
            // C# default double.ToString (no forced .0)
            std::string valstr = JNode::DoubleToString(val);
            if (valstr.size() > 2 && valstr.compare(valstr.size() - 2, 2, ".0") == 0)
                valstr.resize(valstr.size() - 2);
            std::string sb;
            std::string partSb;
            for (char c : valstr) {
                if (('0' <= c && c <= '9') || c == '+' || c == '-') {
                    partSb += c;
                } else {
                    sb += DsonFormatInteger(strtoll(partSb.c_str(), nullptr, 10));
                    partSb.clear();
                    if (c == '.') sb += ".";
                    else if (c == 'e') sb += "very";
                    else if (c == 'E') sb += "VERY";
                }
            }
            sb += DsonFormatInteger(strtoll(partSb.c_str(), nullptr, 10));
            return sb;
        }
        case DT_STR:
            return json->ToString();
        default:
            throw DsonDumpException("unsupported type");
    }
}

// ── IniFileParser ───────────────────────────────────────────────────────────
// JSON5-extra whitespace (same set the JSON parser treats as JSON5 whitespace)
static bool IsIniExtraWhitespace(const std::string& inp, int64_t pos, size_t& cpLen) {
    unsigned char c = static_cast<unsigned char>(inp[static_cast<size_t>(pos)]);
    if (c < 0x80) {
        cpLen = 1;
        return false;
    }
    uint32_t cp = DecodeCodePoint(inp, static_cast<size_t>(pos), cpLen);
    switch (cp) {
        case 0x2028: case 0x2029: case 0xFEFF: case 0xA0: case 0x1680:
        case 0x2000: case 0x2001: case 0x2002: case 0x2003: case 0x2004:
        case 0x2005: case 0x2006: case 0x2007: case 0x2008: case 0x2009:
        case 0x200A: case 0x202F: case 0x205F: case 0x3000:
            return true;
        default:
            return false;
    }
}

std::shared_ptr<JObject> IniFileParser::Parse(const std::string& inp) {
    Reset();
    auto doc = std::make_shared<JObject>();
    int64_t len = static_cast<int64_t>(inp.size());
    while (ii < len) {
        ConsumeInsignificantChars(inp);
        if (ii >= len) break;
        char c = inp[static_cast<size_t>(ii++)];
        if (c == '[') {
            int64_t startOfHeader = ii;
            int64_t headerStartUtf8 = ii - 1;
            int64_t endOfHeader = -1;
            bool headerFinished = false;
            while (ii < len && !headerFinished) {
                c = inp[static_cast<size_t>(ii)];
                switch (c) {
                    case ']':
                        endOfHeader = ii;
                        break;
                    case ' ':
                    case '\t':
                    case '\r':
                        break;
                    case '\n':
                        ii++;
                        lineNum++;
                        if (endOfHeader < 0)
                            throw IniParserException(
                                "Opening '[' and closing ']' of section header were not on the same line",
                                c, headerStartUtf8 - 1, lineNum);
                        headerFinished = true;
                        continue;
                    default: {
                        size_t cpLen;
                        if (IsIniExtraWhitespace(inp, ii, cpLen)) {
                            ii += static_cast<int64_t>(cpLen);
                            continue;
                        }
                        if (endOfHeader > 0)
                            throw IniParserException(
                                "Non-whitespace between end of section header and end of header line",
                                '[', headerStartUtf8 - 1, lineNum);
                        break;
                    }
                }
                ii++;
            }
            if (endOfHeader < 0)
                throw IniParserException(
                    "Opening '[' and closing ']' of section header were not on the same line", c,
                    headerStartUtf8 - 1, lineNum);
            std::string header =
                inp.substr(static_cast<size_t>(startOfHeader), static_cast<size_t>(endOfHeader - startOfHeader));
            if (doc->children.contains(header))
                throw IniParserException("Document has duplicate section header [" + header + "]", c,
                                         headerStartUtf8 - 1, lineNum);
            std::shared_ptr<JObject> section = ParseSection(inp, header);
            section->position = headerStartUtf8;
            doc->children.set(header, section);
        } else {
            throw IniParserException("Expected section header", c, ii, lineNum);
        }
    }
    return doc;
}

std::shared_ptr<JObject> IniFileParser::ParseSection(const std::string& inp,
                                                     const std::string& header) {
    auto section = std::make_shared<JObject>();
    int64_t len = static_cast<int64_t>(inp.size());
    while (ii < len) {
        int64_t indent = ConsumeInsignificantChars(inp);
        int64_t keyStartUtf8 = ii;
        if (ii >= len) break;
        char c = inp[static_cast<size_t>(ii)];
        if (c == '[') break;
        auto kv = ParseKeyValuePair(inp, indent);
        if (section->children.contains(kv.first))
            throw IniParserException("Section [" + header + "] has duplicate key \"" + kv.first + "\"",
                                     c, keyStartUtf8, lineNum);
        section->children.set(kv.first, kv.second);
    }
    return section;
}

std::pair<std::string, JNodePtr> IniFileParser::ParseKeyValuePair(const std::string& inp,
                                                                  int64_t keyIndent) {
    int64_t len = static_cast<int64_t>(inp.size());
    if (ii >= len)
        throw IniParserException("EOF when expecting key-value pair", inp.empty() ? '\0' : inp.back(),
                                 len - 1, lineNum);
    int64_t startOfKey = ii;
    char startC = inp[static_cast<size_t>(ii)];
    int64_t keyStartUtf8 = ii;
    int64_t endOfKey = ii;
    bool foundKeyValueSep = false;
    bool exitKeyLoop = false;
    while (ii < len && !exitKeyLoop) {
        char c = inp[static_cast<size_t>(ii)];
        switch (c) {
            case '=':
            case ':':
                foundKeyValueSep = true;
                break;
            case ' ':
            case '\t':
            case '\r':
                break;
            case '\n':
                if (!foundKeyValueSep)
                    throw IniParserException("No '=' or ':' on same line as key", startC, keyStartUtf8,
                                             lineNum);
                exitKeyLoop = true;
                continue;
            default: {
                size_t cpLen;
                if (IsIniExtraWhitespace(inp, ii, cpLen)) {
                    ii += static_cast<int64_t>(cpLen);
                    continue;
                }
                if (foundKeyValueSep) {
                    exitKeyLoop = true;
                    continue;
                }
                endOfKey = ii;
                break;
            }
        }
        ii++;
    }
    std::string key = inp.substr(static_cast<size_t>(startOfKey),
                                 static_cast<size_t>(endOfKey + 1 - startOfKey));
    int64_t startOfValue = ii;
    int64_t endOfValue = ii;
    while (ii < len) {
        int64_t lineStart = ii;
        ConsumeLine(inp);
        endOfValue = JsonParser::EndOfPreviousLine(inp, ii, lineStart);
        int64_t indent = ConsumeToIndent(inp);
        if (ii < len) {
            if (indent > keyIndent) {
                char c = inp[static_cast<size_t>(ii)];
                if (c == ';' || c == '#') break;
            } else {
                break;
            }
        }
    }
    std::string valueStr = inp.substr(static_cast<size_t>(startOfValue),
                                      static_cast<size_t>(endOfValue - startOfValue));
    if (key.find('=') != std::string::npos || key.find(':') != std::string::npos)
        throw IniParserException("Invalid key is empty or contains ':' or '='",
                                 startOfValue < len ? inp[static_cast<size_t>(startOfValue)] : '\0',
                                 startOfValue, lineNum);
    return {key, JNode::Str(valueStr, keyStartUtf8)};
}

int64_t IniFileParser::ConsumeInsignificantChars(const std::string& inp) {
    int64_t indent = 0;
    int64_t len = static_cast<int64_t>(inp.size());
    while (ii < len) {
        char c = inp[static_cast<size_t>(ii)];
        switch (c) {
            case ' ':
            case '\t':
            case '\r':
                indent++;
                ii++;
                break;
            case '\n':
                lineNum++;
                ii++;
                indent = 0;
                break;
            case ';':
            case '#': {
                int64_t commentStartUtf8 = ii;
                int64_t commentContentStartII = ii + 1;
                ConsumeLine(inp);
                int64_t commentContentEndII =
                    JsonParser::EndOfPreviousLine(inp, ii, commentContentStartII);
                comments.emplace_back(
                    inp.substr(static_cast<size_t>(commentContentStartII),
                               static_cast<size_t>(commentContentEndII - commentContentStartII)),
                    false, commentStartUtf8);
                indent = 0;
                break;
            }
            default: {
                size_t cpLen;
                if (IsIniExtraWhitespace(inp, ii, cpLen)) {
                    indent++;
                    ii += static_cast<int64_t>(cpLen);
                    break;
                }
                return indent;
            }
        }
    }
    return 0;
}

void IniFileParser::ConsumeLine(const std::string& inp) {
    int64_t len = static_cast<int64_t>(inp.size());
    while (ii < len) {
        char c = inp[static_cast<size_t>(ii++)];
        if (c == '\n') {
            lineNum++;
            return;
        }
    }
}

int64_t IniFileParser::ConsumeToIndent(const std::string& inp) {
    int64_t indent = 0;
    int64_t len = static_cast<int64_t>(inp.size());
    while (ii < len) {
        char c = inp[static_cast<size_t>(ii)];
        switch (c) {
            case ' ':
            case '\t':
            case '\r':
                break;
            case '\n':
                ii++;
                lineNum++;
                return 0;
            default: {
                size_t cpLen;
                if (IsIniExtraWhitespace(inp, ii, cpLen)) {
                    ii += static_cast<int64_t>(cpLen);
                    indent++;
                    continue;
                }
                return indent;
            }
        }
        ii++;
        indent++;
    }
    return 0;
}

// ── CsvSniffer ──────────────────────────────────────────────────────────────
int64_t CsvSniffer::Sniff(const std::string& text, EndOfLine eol, char delimiter, char quote,
                          int64_t maxLinesToSniff, int64_t maxCharsToSniff,
                          int64_t minLinesToDecide) {
    int64_t textLen = static_cast<int64_t>(text.size());
    maxCharsToSniff = maxCharsToSniff > textLen ? textLen : maxCharsToSniff;
    int64_t nColumns = -1;
    int64_t nColumnsThisLine = 0;
    int64_t matchStart = 0;
    int64_t linesConsumed = 0;
    std::string newline = eol == EndOfLine::CRLF ? "\r\n" : eol == EndOfLine::CR ? "\r" : "\n";
    std::string escapedNewline = JNode::StrToString(newline, false);
    // build the same per-column regex s_csv uses
    auto cleanChar = [](char c) -> std::string {
        if (c == '\t') return "\\t";
        static const std::string special = "\\^$.|?*+()[]{}";
        if (special.find(c) != std::string::npos) return std::string("\\") + c;
        return std::string(1, c);
    };
    std::string delimStr = cleanChar(delimiter);
    std::string quoteStr = cleanChar(quote);
    std::string column = "([^" "{DELIM}" "\\r\\n" "{QUOTE}" "]*|{QUOTE}(?:[^{QUOTE}]|{QUOTE}{2})*{QUOTE})";
    size_t pos;
    while ((pos = column.find("{QUOTE}")) != std::string::npos) column.replace(pos, 7, quoteStr);
    while ((pos = column.find("{DELIM}")) != std::string::npos) column.replace(pos, 7, delimStr);
    std::string newlineOrDelimiter = "(?:" + delimStr + "|" + escapedNewline + "|\\z)";
    Regex regex(column + newlineOrDelimiter, false, false);
    while (matchStart < maxCharsToSniff) {
        Regex::Match match = regex.FirstMatch(text, matchStart);
        if (!match.success) return -1;
        nColumnsThisLine++;
        int64_t matchEnd = matchStart + match.length;
        if (matchEnd == matchStart) matchEnd++;
        bool atEndOfLine = matchEnd >= textLen ||
            (match.value.size() >= newline.size() &&
             match.value.compare(match.value.size() - newline.size(), newline.size(), newline) == 0);
        if (atEndOfLine) {
            if (nColumns == -1) {
                nColumns = nColumnsThisLine;
            } else if (nColumns == 1 || (nColumns >= 0 && nColumnsThisLine != nColumns)) {
                return -1;
            }
            nColumnsThisLine = 0;
            linesConsumed++;
            if (linesConsumed == maxLinesToSniff) return nColumns;
        }
        matchStart = matchEnd;
    }
    if (linesConsumed < 2 || (matchStart < textLen && linesConsumed < minLinesToDecide)) return -1;
    return nColumns;
}

} // namespace jsontools
