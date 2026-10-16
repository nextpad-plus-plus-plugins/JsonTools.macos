// JNode.cpp — see JNode.h for the port notes.

#include "JNode.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <xlocale.h>

namespace jsontools {

std::string JNode::NL = "\n";

// defined in RemesPathFunctions.cpp (EngineGlobals.cpp until that lands)
void ArgFunctionInitializeGlobals(bool mutatesInput);
extern bool regexSearchResultsShouldBeCached;

// ── Dtype strings ──────────────────────────────────────────────────────────
std::string FormatDtype(Dtype type) {
    // mixed types come first so they're checked before pure ones (C# parity)
    static const std::pair<Dtype, const char*> kDtypeStrings[] = {
        {DT_SCALAR, "scalar"}, {DT_ITERABLE, "iterable"}, {DT_NUM, "numeric"},
        {DT_ARR, "array"}, {DT_BOOL, "boolean"}, {DT_FLOAT, "float"},
        {DT_INT, "integer"}, {DT_NULL, "null"}, {DT_OBJ, "object"},
        {DT_STR, "string"}, {DT_UNKNOWN, "unknown"}, {DT_SLICE, "slice"},
        {DT_REGEX, "regex"},
    };
    for (auto& kv : kDtypeStrings)
        if (kv.first == type) return kv.second;
    std::string out;
    uint16_t typeint = static_cast<uint16_t>(type);
    for (auto& kv : kDtypeStrings) {
        uint16_t shortyp = static_cast<uint16_t>(kv.first);
        if ((typeint & shortyp) == shortyp && shortyp != 0) {
            if (!out.empty()) out += '|';
            out += kv.second;
            typeint -= shortyp;
        }
    }
    return out;
}

// ── key sorting ────────────────────────────────────────────────────────────
// KeyLessIgnoreCase lives in Regex.mm: it needs CFString collation to match
// the C# StringComparer.CurrentCultureIgnoreCase ordering (e.g. "baßk" must
// sort before "basst").

static std::vector<std::string> SortedKeys(const OrderedMap& children) {
    std::vector<std::string> keys = children.keys();
    std::sort(keys.begin(), keys.end(), KeyLessIgnoreCase);
    return keys;
}

// ── OrderedMap ─────────────────────────────────────────────────────────────
JNodePtr* OrderedMap::find(const std::string& k) {
    auto it = index_.find(k);
    return it == index_.end() ? nullptr : &items_[it->second].second;
}
const JNodePtr* OrderedMap::find(const std::string& k) const {
    auto it = index_.find(k);
    return it == index_.end() ? nullptr : &items_[it->second].second;
}
void OrderedMap::set(const std::string& k, JNodePtr v) {
    auto it = index_.find(k);
    if (it == index_.end()) {
        index_[k] = items_.size();
        items_.emplace_back(k, std::move(v));
    } else {
        items_[it->second].second = std::move(v);
    }
}
JNodePtr& OrderedMap::operator[](const std::string& k) {
    auto it = index_.find(k);
    if (it == index_.end()) {
        index_[k] = items_.size();
        items_.emplace_back(k, nullptr);
        return items_.back().second;
    }
    return items_[it->second].second;
}
JNodePtr OrderedMap::at(const std::string& k) const {
    auto it = index_.find(k);
    if (it == index_.end()) throw std::out_of_range("key not found: " + k);
    return items_[it->second].second;
}
bool OrderedMap::erase(const std::string& k) {
    auto it = index_.find(k);
    if (it == index_.end()) return false;
    size_t pos = it->second;
    items_.erase(items_.begin() + static_cast<long>(pos));
    index_.erase(it);
    for (auto& kv : index_)
        if (kv.second > pos) kv.second--;
    return true;
}
std::vector<std::string> OrderedMap::keys() const {
    std::vector<std::string> out;
    out.reserve(items_.size());
    for (auto& kv : items_) out.push_back(kv.first);
    return out;
}

// ── Comment ────────────────────────────────────────────────────────────────
int64_t Comment::ToStringHelper(std::string& sb, const Comment& c,
                                const std::string& singleLineCommentStart) {
    sb += c.isMultiline ? "/*" : singleLineCommentStart;
    sb += c.content;
    if (c.isMultiline) sb += "*/";
    sb += JNode::NL;
    return 0;
}

size_t Comment::AppendAllCommentsBeforePosition(std::string& sb,
                                                std::vector<Comment>& comments,
                                                size_t commentIdx, int64_t position,
                                                const std::string& dent,
                                                DocumentType commentType) {
    std::string singleLineCommentStart;
    switch (commentType) {
        case DocumentType::JSON:
        case DocumentType::JSONL: singleLineCommentStart = "//"; break;
        case DocumentType::INI:   singleLineCommentStart = ";"; break;
        default: throw JsonToolsError("not a valid comment type");
    }
    for (; commentIdx < comments.size(); commentIdx++) {
        const Comment& comment = comments[commentIdx];
        if (comment.position > position) break;
        sb += dent;
        ToStringHelper(sb, comment, singleLineCommentStart);
    }
    return commentIdx;
}

static void SortCommentsByPosition(std::vector<Comment>& comments) {
    std::stable_sort(comments.begin(), comments.end(),
                     [](const Comment& x, const Comment& y) { return x.position < y.position; });
}

// ── char/string escaping ───────────────────────────────────────────────────
void JNode::CharToSb(std::string& sb, unsigned char c) {
    switch (c) {
        case '\\': sb += "\\\\"; return;
        case '"':  sb += "\\\""; return;
        case '\b': sb += "\\b"; return;
        case '\t': sb += "\\t"; return;
        case '\n': sb += "\\n"; return;
        case '\v': sb += "\\v"; return;
        case '\f': sb += "\\f"; return;
        case '\r': sb += "\\r"; return;
        default:
            if (c < 0x20) {
                char buf[8];
                snprintf(buf, sizeof buf, "\\u%04X", c);
                sb += buf;
            } else {
                sb += static_cast<char>(c);
            }
            return;
    }
}

std::string JNode::StrToString(const std::string& s, bool quoted) {
    size_t slen = s.size(), ii = 0;
    for (; ii < slen; ii++) {
        unsigned char c = static_cast<unsigned char>(s[ii]);
        if (c < 32 || c == '\\' || c == '"') break;
    }
    if (ii == slen) return quoted ? "\"" + s + "\"" : s;
    std::string sb;
    sb.reserve(slen + 8);
    if (quoted) sb += '"';
    sb.append(s, 0, ii);
    for (; ii < slen; ii++) CharToSb(sb, static_cast<unsigned char>(s[ii]));
    if (quoted) sb += '"';
    return sb;
}

void JNode::StrToSb(std::string& sb, const std::string& s) {
    size_t slen = s.size(), ii = 0;
    for (; ii < slen; ii++) {
        unsigned char c = static_cast<unsigned char>(s[ii]);
        if (c < 32 || c == '\\' || c == '"') break;
    }
    if (ii == slen) { sb += s; return; }
    sb.append(s, 0, ii);
    for (; ii < slen; ii++) CharToSb(sb, static_cast<unsigned char>(s[ii]));
}

// C# double.ToString("R"): .NET Framework's round-trip format first tries 15
// significant digits and falls back to 17 (never 16!) if that doesn't
// round-trip. Fixed notation for decimal exponents in [-4, 15), otherwise
// scientific like "1.5E-05"/"1E+17" with an uppercase E, explicit sign, and
// at least two exponent digits.
std::string JNode::DoubleToString(double d) {
    // _l variants with a fixed C locale: the user's locale may use ',' as the
    // decimal separator (upstream issue #17), but JSON is always '.'
    static locale_t cLocale = newlocale(LC_ALL_MASK, "C", nullptr);
    char shortest[64];
    snprintf_l(shortest, sizeof shortest, cLocale, "%.14e", d); // 15 significant digits
    if (strtod_l(shortest, nullptr, cLocale) != d)
        snprintf_l(shortest, sizeof shortest, cLocale, "%.16e", d); // 17 significant digits
    std::string t(shortest);
    // "%.14e" gives e.g. "1.00000000000000e+17", "3.14000000000000e+00";
    // trailing zeros are stripped below
    size_t epos = t.find('e');
    std::string digits;   // mantissa digits without sign or '.'
    int decExp;           // exponent such that value = 0.D1D2... * 10^decExp? use normalized below
    bool neg = !t.empty() && t[0] == '-';
    std::string mant = epos == std::string::npos ? t : t.substr(0, epos);
    if (neg) mant.erase(0, 1);
    int expPart = epos == std::string::npos ? 0 : std::stoi(t.substr(epos + 1));
    size_t dot = mant.find('.');
    std::string intPart = dot == std::string::npos ? mant : mant.substr(0, dot);
    std::string fracPart = dot == std::string::npos ? "" : mant.substr(dot + 1);
    digits = intPart + fracPart;
    // exponent of the first digit (10^k place of leading digit)
    int leadExp = static_cast<int>(intPart.size()) - 1 + expPart;
    // strip leading zeros in digits (e.g. "0.5" → digits "05")
    size_t firstNonZero = digits.find_first_not_of('0');
    if (firstNonZero == std::string::npos) return neg ? "-0.0" : "0.0";
    leadExp -= static_cast<int>(firstNonZero);
    digits.erase(0, firstNonZero);
    // strip trailing zeros
    size_t lastNonZero = digits.find_last_not_of('0');
    digits.erase(lastNonZero + 1);
    int ndigits = static_cast<int>(digits.size());

    std::string out;
    if (leadExp >= 15 || leadExp < -4) {
        // scientific: D.DDD...E±XX
        out = digits.substr(0, 1);
        if (ndigits > 1) { out += '.'; out += digits.substr(1); }
        out += 'E';
        out += leadExp < 0 ? '-' : '+';
        int ae = leadExp < 0 ? -leadExp : leadExp;
        if (ae < 10) out += '0';
        out += std::to_string(ae);
        if (neg) out.insert(0, "-");
        return out;
    }
    if (leadExp >= ndigits - 1) {
        // integer-valued in fixed notation
        out = digits;
        out.append(static_cast<size_t>(leadExp - (ndigits - 1)), '0');
        out += ".0"; // distinguish float from int (C# appends it when no E)
        if (neg) out.insert(0, "-");
        return out;
    }
    if (leadExp >= 0) {
        out = digits.substr(0, static_cast<size_t>(leadExp + 1)) + "." +
              digits.substr(static_cast<size_t>(leadExp + 1));
    } else {
        out = "0.";
        out.append(static_cast<size_t>(-leadExp - 1), '0');
        out += digits;
    }
    if (neg) out.insert(0, "-");
    return out;
}

double JNode::ToDouble() const {
    switch (type) {
        case DT_BOOL:  return GetBool() ? 1.0 : 0.0;
        case DT_INT:   return static_cast<double>(GetInt());
        case DT_FLOAT: return GetDouble();
        default: throw ComparisonError("cannot convert JNode of type " + FormatDtype(type) + " to number");
    }
}

// ── compact printing ───────────────────────────────────────────────────────
std::string JNode::ToString(bool /*sortKeys*/, const std::string& /*keyValueSep*/,
                            const std::string& /*itemSep*/, int64_t /*maxLength*/) const {
    switch (type) {
        case DT_STR: return StrToString(GetStr(), true);
        case DT_FLOAT: {
            double v = GetDouble();
            if (std::isinf(v)) return v < 0 ? "-Infinity" : "Infinity";
            if (std::isnan(v)) return "NaN";
            return DoubleToString(v);
        }
        case DT_INT: return std::to_string(GetInt());
        case DT_NULL: return "null";
        case DT_BOOL: return GetBool() ? "true" : "false";
        default: return "JNode(type=" + FormatDtype(type) + ")";
    }
}

std::string JRegex::ToString(bool, const std::string&, const std::string&, int64_t) const {
    return StrToString(regex->Pattern(), true);
}

int JNode::ToStringHelper(bool sortKeys, const std::string& keyValueSep,
                          const std::string& itemSep, std::string& sb,
                          bool changePositions, int64_t /*maxLength*/) {
    (void)sortKeys; (void)keyValueSep; (void)itemSep;
    if (changePositions) position = static_cast<int64_t>(sb.size());
    sb += ToString();
    return 0;
}

// ── pretty printing (scalar fallbacks) ─────────────────────────────────────
std::string JNode::PrettyPrint(int, bool, PrettyPrintStyle, int64_t, char) {
    return ToString();
}
int JNode::PrettyPrintHelper(int, bool sortKeys, PrettyPrintStyle, int, std::string& sb,
                             bool changePositions, int64_t maxLength, char) {
    return ToStringHelper(sortKeys, ": ", ", ", sb, changePositions, maxLength);
}
std::string JNode::ToStringAndChangePositions(bool sortKeys, const std::string& keyValueSep,
                                              const std::string& itemSep, int64_t maxLength) {
    position = 0;
    return ToString(sortKeys, keyValueSep, itemSep, maxLength);
}
std::string JNode::PrettyPrintAndChangePositions(int indent, bool sortKeys,
                                                 PrettyPrintStyle style, int64_t maxLength,
                                                 char indentChar) {
    position = 0;
    return PrettyPrint(indent, sortKeys, style, maxLength, indentChar);
}
int JNode::PPrintHelper(int, int, bool sortKeys, std::string& sb, bool changePositions,
                        int64_t, int64_t, char) {
    return ToStringHelper(sortKeys, ":", ",", sb, changePositions, I64_MAX);
}

// ── printing with comments ─────────────────────────────────────────────────
std::string JNode::ToStringWithComments(std::vector<Comment>& comments, bool sortKeys) {
    SortCommentsByPosition(comments);
    std::string sb;
    Comment::AppendAllCommentsBeforePosition(sb, comments, 0, I64_MAX, "");
    ToStringHelper(sortKeys, ": ", ", ", sb, false, I64_MAX);
    return sb;
}

std::string JNode::ToStringWithCommentsAndChangePositions(std::vector<Comment>& comments,
                                                          bool sortKeys) {
    SortCommentsByPosition(comments);
    std::string sb;
    Comment::AppendAllCommentsBeforePosition(sb, comments, 0, I64_MAX, "");
    position = static_cast<int64_t>(sb.size());
    ToStringHelper(sortKeys, ": ", ", ", sb, true, I64_MAX);
    return sb;
}

std::string JNode::PrettyPrintWithComments(std::vector<Comment>& comments, int indent,
                                           bool sortKeys, char indentChar,
                                           PrettyPrintStyle prettyPrintStyle) {
    SortCommentsByPosition(comments);
    bool pprint = prettyPrintStyle == PrettyPrintStyle::PPrint;
    std::string sb;
    size_t commentIdx = Comment::AppendAllCommentsBeforePosition(sb, comments, 0, position, "");
    commentIdx = PrettyPrintWithCommentsHelper(comments, commentIdx, indent, sortKeys, 0, sb,
                                               false, indentChar, pprint);
    sb += NL;
    Comment::AppendAllCommentsBeforePosition(sb, comments, commentIdx, I64_MAX, "");
    return sb;
}

std::string JNode::PrettyPrintWithCommentsAndChangePositions(std::vector<Comment>& comments,
                                                             int indent, bool sortKeys,
                                                             char indentChar,
                                                             PrettyPrintStyle prettyPrintStyle) {
    SortCommentsByPosition(comments);
    bool pprint = prettyPrintStyle == PrettyPrintStyle::PPrint;
    std::string sb;
    size_t commentIdx = Comment::AppendAllCommentsBeforePosition(sb, comments, 0, position, "");
    commentIdx = PrettyPrintWithCommentsHelper(comments, commentIdx, indent, sortKeys, 0, sb,
                                               true, indentChar, pprint);
    sb += NL;
    Comment::AppendAllCommentsBeforePosition(sb, comments, commentIdx, I64_MAX, "");
    return sb;
}

size_t JNode::PrettyPrintWithCommentsHelper(std::vector<Comment>& comments, size_t commentIdx,
                                            int /*indent*/, bool sortKeys, int /*depth*/,
                                            std::string& sb, bool changePositions,
                                            char /*indentChar*/, bool /*pprint*/) {
    ToStringHelper(sortKeys, ": ", ", ", sb, changePositions, I64_MAX);
    return commentIdx;
}

size_t JNode::PPrintWithCommentsHelper(std::vector<Comment>& comments, size_t commentIdx,
                                       int /*indent*/, bool sortKeys, int /*depth*/,
                                       std::string& sb, bool changePositions,
                                       char /*indentChar*/, int64_t /*maxSbLen*/) {
    ToStringHelper(sortKeys, ": ", ", ", sb, changePositions, I64_MAX);
    return commentIdx;
}

int64_t JNode::CompressedLengthAndPositionsWithinThresholds(int64_t sbLength,
                                                            int64_t maxInitialPosition,
                                                            int64_t maxSbLen) const {
    if (position >= maxInitialPosition || sbLength >= maxSbLen) return -1;
    return sbLength + static_cast<int64_t>(ToString().size());
}

// ── comparison / equality / copy ───────────────────────────────────────────
int JNode::CompareTo(const JNode& other) const { return CompareTo(other.value); }

static int CmpDouble(double a, double b) { return a < b ? -1 : (a > b ? 1 : 0); }

int JNode::CompareTo(const JValue& other) const {
    switch (type) {
        case DT_STR: {
            if (!std::holds_alternative<std::string>(other))
                throw ComparisonError("Can't compare strings to non-strings");
            const std::string& a = GetStr();
            const std::string& b = std::get<std::string>(other);
            int c = a.compare(b);
            return c < 0 ? -1 : (c > 0 ? 1 : 0);
        }
        case DT_INT:
        case DT_FLOAT:
        case DT_BOOL: {
            double a = ToDouble();
            double b;
            if (std::holds_alternative<int64_t>(other)) b = static_cast<double>(std::get<int64_t>(other));
            else if (std::holds_alternative<double>(other)) b = std::get<double>(other);
            else if (std::holds_alternative<bool>(other)) b = std::get<bool>(other) ? 1.0 : 0.0;
            else throw ComparisonError("Can't compare numbers to non-numbers");
            return CmpDouble(a, b);
        }
        case DT_NULL:
            if (!std::holds_alternative<std::monostate>(other))
                throw ComparisonError("Cannot compare null to non-null");
            return 0;
        case DT_ARR:
        case DT_OBJ:
            throw ComparisonError("Cannot compare JArrays or JObjects");
        default:
            throw ComparisonError("Cannot compare JNodes of type " + FormatDtype(type));
    }
}

bool JNode::Equals(const JNode& other) const { return CompareTo(other) == 0; }

bool JNode::TryEquals(const JNode& other, std::string* errorMessage) const {
    try {
        return Equals(other);
    } catch (const std::exception& ex) {
        if (errorMessage) *errorMessage = ex.what();
        return false;
    }
}

JNodePtr JNode::Copy() const { return std::make_shared<JNode>(value, type, position); }

// ── tree navigation ────────────────────────────────────────────────────────
bool JNode::ParentHierarchyHelper(const JNodePtr& current, const JNode* target,
                                  std::vector<PathStep>& keys, std::vector<JNodePtr>& parents) {
    if (current.get() == target) return true;
    if (JArray* arr = AsArr(current)) {
        for (size_t ii = 0; ii < arr->children.size(); ii++) {
            parents.push_back(current);
            PathStep st; st.isIndex = true; st.index = static_cast<int64_t>(ii);
            keys.push_back(st);
            if (ParentHierarchyHelper(arr->children[ii], target, keys, parents)) return true;
            keys.pop_back();
            parents.pop_back();
        }
    } else if (JObject* obj = AsObj(current)) {
        for (auto& kv : obj->children) {
            parents.push_back(current);
            PathStep st; st.key = kv.first;
            keys.push_back(st);
            if (ParentHierarchyHelper(kv.second, target, keys, parents)) return true;
            keys.pop_back();
            parents.pop_back();
        }
    }
    return false;
}

std::vector<std::pair<JNode::PathStep, JNodePtr>> JNode::ParentHierarchy(const JNodePtr& root) {
    std::vector<PathStep> keys;
    std::vector<JNodePtr> parents;
    ParentHierarchyHelper(root, this, keys, parents);
    std::vector<std::pair<PathStep, JNodePtr>> out;
    for (size_t i = 0; i < keys.size(); i++) out.emplace_back(keys[i], parents[i]);
    return out;
}

bool JNode::ContainsPosition(int64_t pos) const {
    if (position == pos) return true;
    if ((type & DT_ARR_OR_OBJ) != 0) return false;
    int64_t len = static_cast<int64_t>(ToString().size());
    return pos > position && pos <= position + len;
}

bool JNode::DotCompatible(const std::string& key) {
    if (key.empty()) return false;
    unsigned char c0 = static_cast<unsigned char>(key[0]);
    if (!(c0 == '_' || (c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z'))) return false;
    for (size_t i = 1; i < key.size(); i++) {
        unsigned char c = static_cast<unsigned char>(key[i]);
        if (!(c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
            return false;
    }
    return true;
}

static std::string ReplaceAll(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

void JNode::ThrowIfPathSeparatorInvalid(char sep) {
    if (sep == '"' || (sep >= '0' && sep <= '9'))
        throw JsonToolsError("separator CANNOT be any of the characters in the following JSON string: \"\\\"0123456789\"");
}

std::string JNode::FormatKey(const std::string& key, KeyStyle style, char separator) {
    if (separator != DEFAULT_PATH_SEPARATOR) {
        ThrowIfPathSeparatorInvalid(separator);
        return key.find(separator) == std::string::npos && DotCompatible(key)
            ? std::string(1, separator) + key
            : std::string(1, separator) + StrToString(key, true);
    }
    switch (style) {
        case KeyStyle::RemesPath: {
            if (DotCompatible(key)) return "." + key;
            std::string escapedKey = StrToString(key, false);
            std::string keyDubquotesUnescaped =
                ReplaceAll(ReplaceAll(escapedKey, "\\\"", "\""), "`", "\\`");
            return "[`" + keyDubquotesUnescaped + "`]";
        }
        case KeyStyle::JavaScript: {
            if (DotCompatible(key)) return "." + key;
            std::string escapedKey = StrToString(key, false);
            if (key.find('\'') != std::string::npos) return "[\"" + escapedKey + "\"]";
            return "['" + ReplaceAll(escapedKey, "\\\"", "\"") + "']";
        }
        case KeyStyle::Python: {
            std::string escapedKey = StrToString(key, false);
            if (escapedKey.find('\'') != std::string::npos) return "[\"" + escapedKey + "\"]";
            return "['" + ReplaceAll(escapedKey, "\\\"", "\"") + "']";
        }
    }
    throw JsonToolsError("style argument for FormatKey must be a KeyStyle member");
}

std::string JNode::FormatIndex(int64_t idx, char separator) {
    return separator == DEFAULT_PATH_SEPARATOR
        ? "[" + std::to_string(idx) + "]"
        : std::string(1, separator) + std::to_string(idx);
}

static std::string FormatPath(const std::vector<JNode::PathStep>& path, KeyStyle style,
                              char separator) {
    std::string sb;
    for (const auto& member : path) {
        if (member.isIndex) sb += JNode::FormatIndex(member.index, separator);
        else sb += JNode::FormatKey(member.key, style, separator);
    }
    return sb;
}

std::string JNode::PathToPosition(int64_t pos, KeyStyle style, char separator) {
    std::vector<PathStep> path;
    return PathToPositionHelper(pos, style, path, separator);
}

std::string JNode::PathToPositionHelper(int64_t pos, KeyStyle style,
                                        std::vector<PathStep>& path, char separator) {
    if (ContainsPosition(pos)) return FormatPath(path, style, separator);
    if (type == DT_ARR) {
        JArray* arr = static_cast<JArray*>(this);
        if (arr->Length() == 0) return "";
        size_t ii = 0;
        while (ii < arr->Length() - 1 && arr->children[ii + 1]->position <= pos) ii++;
        JNodePtr child = arr->children[ii];
        PathStep st; st.isIndex = true; st.index = static_cast<int64_t>(ii);
        path.push_back(st);
        std::string result = child->PathToPositionHelper(pos, style, path, separator);
        if (!result.empty()) return result;
        path.pop_back();
    } else if (type == DT_OBJ) {
        JObject* obj = static_cast<JObject*>(this);
        for (auto& kv : obj->children) {
            PathStep st; st.key = kv.first;
            path.push_back(st);
            std::string result = kv.second->PathToPositionHelper(pos, style, path, separator);
            if (!result.empty()) return result;
            path.pop_back();
        }
    }
    return "";
}

void JNode::MutateInto(const JNode& vnew) {
    if (type == DT_ARR) throw InvalidMutationError("Can't mutate an array.");
    if (type == DT_OBJ) throw InvalidMutationError("Can't mutate an object.");
    if ((vnew.type & DT_ARR_OR_OBJ) != 0)
        throw InvalidMutationError("Can't convert a scalar to an array or object.");
    type = vnew.type;
    value = vnew.value;
}

// ── JObject ────────────────────────────────────────────────────────────────
bool JObject::TryGetValue(const std::string& key, JNodePtr& val) const {
    const JNodePtr* p = children.find(key);
    if (!p) { val = nullptr; return false; }
    val = *p;
    return true;
}

bool JObject::TryGetString(const std::string& key, std::string& val) const {
    const JNodePtr* p = children.find(key);
    if (p && *p && (*p)->IsStrValue()) { val = (*p)->GetStr(); return true; }
    return false;
}

std::string JObject::ToString(bool sortKeys, const std::string& keyValueSep,
                              const std::string& itemSep, int64_t maxLength) const {
    std::string sb;
    sb.reserve(7 * Length());
    const_cast<JObject*>(this)->ToStringHelper(sortKeys, keyValueSep, itemSep, sb, false, maxLength);
    if (static_cast<int64_t>(sb.size()) >= maxLength) sb += "...";
    return sb;
}

int JObject::ToStringHelper(bool sortKeys, const std::string& keyValueSep,
                            const std::string& itemSep, std::string& sb,
                            bool changePositions, int64_t maxLength) {
    if (static_cast<int64_t>(sb.size()) >= maxLength) return -1;
    if (changePositions) position = static_cast<int64_t>(sb.size());
    sb += '{';
    size_t ctr = 0;
    std::vector<std::string> keys = sortKeys ? SortedKeys(children) : children.keys();
    for (const std::string& k : keys) {
        JNodePtr v = children.at(k);
        sb += '"';
        StrToSb(sb, k);
        sb += '"';
        sb += keyValueSep;
        if (v->ToStringHelper(sortKeys, keyValueSep, itemSep, sb, changePositions, maxLength) == -1 ||
            static_cast<int64_t>(sb.size()) >= maxLength)
            return -1;
        if (++ctr < children.size()) sb += itemSep;
    }
    sb += '}';
    return 0;
}

std::string JObject::PrettyPrint(int indent, bool sortKeys, PrettyPrintStyle style,
                                 int64_t maxLength, char indentChar) {
    std::string sb;
    sb.reserve(8 * Length());
    PrettyPrintHelper(indent, sortKeys, style, 0, sb, false, maxLength, indentChar);
    if (static_cast<int64_t>(sb.size()) >= maxLength) sb += "...";
    return sb;
}

static int AppendKey(std::string& sb, const std::string& dent, const std::string& k,
                     const char* closeQuoteAndColon) {
    sb += dent;
    sb += '"';
    JNode::StrToSb(sb, k);
    sb += closeQuoteAndColon;
    return 0;
}

int JObject::PrettyPrintHelper(int indent, bool sortKeys, PrettyPrintStyle style, int depth,
                               std::string& sb, bool changePositions, int64_t maxLength,
                               char indentChar) {
    if (static_cast<int64_t>(sb.size()) >= maxLength) return -1;
    std::string dent(static_cast<size_t>(indent * depth), indentChar);
    size_t ctr = 0;
    std::vector<std::string> keys = sortKeys ? SortedKeys(children) : children.keys();
    switch (style) {
        case PrettyPrintStyle::Whitesmith: {
            sb += dent;
            if (changePositions) position = static_cast<int64_t>(sb.size());
            sb += '{';
            sb += NL;
            for (const std::string& k : keys) {
                JNodePtr v = children.at(k);
                AppendKey(sb, dent, k, "\":");
                if (v->type == DT_OBJ || v->type == DT_ARR) sb += NL;
                else sb += ' ';
                if (v->PrettyPrintHelper(indent, sortKeys, style, depth + 1, sb, changePositions,
                                         maxLength, indentChar) == -1 ||
                    static_cast<int64_t>(sb.size()) >= maxLength)
                    return -1;
                if (++ctr < children.size()) sb += ',';
                sb += NL;
            }
            sb += dent;
            sb += '}';
            break;
        }
        case PrettyPrintStyle::Google: {
            if (changePositions) position = static_cast<int64_t>(sb.size());
            sb += '{';
            sb += NL;
            std::string extraDent(static_cast<size_t>((depth + 1) * indent), indentChar);
            for (const std::string& k : keys) {
                JNodePtr v = children.at(k);
                AppendKey(sb, extraDent, k, "\": ");
                if (v->PrettyPrintHelper(indent, sortKeys, style, depth + 1, sb, changePositions,
                                         maxLength, indentChar) == -1 ||
                    static_cast<int64_t>(sb.size()) >= maxLength)
                    return -1;
                if (++ctr < children.size()) sb += ',';
                sb += NL;
            }
            sb += dent;
            sb += '}';
            break;
        }
        case PrettyPrintStyle::PPrint: {
            if (changePositions) position = static_cast<int64_t>(sb.size());
            sb += '{';
            sb += NL;
            std::string extraDent(static_cast<size_t>((depth + 1) * indent), indentChar);
            for (const std::string& k : keys) {
                int64_t maxLineEnd = static_cast<int64_t>(sb.size()) + PPRINT_LINE_LENGTH;
                JNodePtr v = children.at(k);
                AppendKey(sb, extraDent, k, "\": ");
                if (v->PPrintHelper(indent, depth, sortKeys, sb, changePositions, maxLineEnd,
                                    maxLength, indentChar) == -1 ||
                    static_cast<int64_t>(sb.size()) >= maxLength)
                    return -1;
                if (++ctr < children.size()) sb += ',';
                sb += NL;
            }
            sb += dent;
            sb += '}';
            break;
        }
    }
    return 0;
}

std::string JObject::ToStringAndChangePositions(bool sortKeys, const std::string& keyValueSep,
                                                const std::string& itemSep, int64_t maxLength) {
    std::string sb;
    sb.reserve(7 * Length());
    ToStringHelper(sortKeys, keyValueSep, itemSep, sb, true, maxLength);
    if (static_cast<int64_t>(sb.size()) >= maxLength) sb += "...";
    return sb;
}

std::string JObject::PrettyPrintAndChangePositions(int indent, bool sortKeys,
                                                   PrettyPrintStyle style, int64_t maxLength,
                                                   char indentChar) {
    std::string sb;
    sb.reserve(8 * Length());
    PrettyPrintHelper(indent, sortKeys, style, 0, sb, true, maxLength, indentChar);
    if (static_cast<int64_t>(sb.size()) >= maxLength) sb += "...";
    return sb;
}

int JObject::PPrintHelper(int indent, int depth, bool sortKeys, std::string& sb,
                          bool changePositions, int64_t maxLineEnd, int64_t maxLength,
                          char indentChar) {
    // a non-minimal-whitespace-compressed object has >= 8 chars per element
    if (static_cast<int64_t>(Length()) > PPRINT_LINE_LENGTH / 8)
        return PrettyPrintHelper(indent, sortKeys, PrettyPrintStyle::PPrint, depth + 1, sb,
                                 changePositions, maxLength, indentChar);
    size_t ogSbLen = sb.size();
    int result = ToStringHelper(sortKeys, ": ", ", ", sb, changePositions, maxLineEnd);
    if (result == -1) {
        sb.resize(ogSbLen);
        return PrettyPrintHelper(indent, sortKeys, PrettyPrintStyle::PPrint, depth + 1, sb,
                                 changePositions, maxLength, indentChar);
    }
    return result;
}

size_t JObject::PrettyPrintWithCommentsHelper(std::vector<Comment>& comments, size_t commentIdx,
                                              int indent, bool sortKeys, int depth,
                                              std::string& sb, bool changePositions,
                                              char indentChar, bool pprint) {
    if (changePositions) position = static_cast<int64_t>(sb.size());
    size_t ctr = 0;
    std::vector<std::string> keys = sortKeys ? SortedKeys(children) : children.keys();
    sb += '{';
    sb += NL;
    std::string dent(static_cast<size_t>(indent * depth), indentChar);
    std::string extraDent(static_cast<size_t>((depth + 1) * indent), indentChar);
    for (const std::string& k : keys) {
        JNodePtr v = children.at(k);
        commentIdx = Comment::AppendAllCommentsBeforePosition(sb, comments, commentIdx,
                                                              v->position, extraDent);
        int64_t pprintLineEnd = static_cast<int64_t>(sb.size()) + PPRINT_LINE_LENGTH;
        AppendKey(sb, extraDent, k, "\": ");
        commentIdx = pprint
            ? v->PPrintWithCommentsHelper(comments, commentIdx, indent, sortKeys, depth + 1, sb,
                                          changePositions, indentChar, pprintLineEnd)
            : v->PrettyPrintWithCommentsHelper(comments, commentIdx, indent, sortKeys, depth + 1,
                                               sb, changePositions, indentChar, false);
        if (++ctr < children.size()) sb += ',';
        sb += NL;
    }
    sb += dent;
    sb += '}';
    return commentIdx;
}

size_t JObject::PPrintWithCommentsHelper(std::vector<Comment>& comments, size_t commentIdx,
                                         int indent, bool sortKeys, int depth, std::string& sb,
                                         bool changePositions, char indentChar,
                                         int64_t maxSbLen) {
    if (static_cast<int64_t>(Length()) <= PPRINT_LINE_LENGTH / 8) {
        int64_t ogSbLen = static_cast<int64_t>(sb.size());
        int64_t nextCommentPos = commentIdx >= comments.size() ? I64_MAX : comments[commentIdx].position;
        if (CompressedLengthAndPositionsWithinThresholds(ogSbLen, nextCommentPos, maxSbLen) != -1) {
            ToStringHelper(sortKeys, ": ", ", ", sb, changePositions, I64_MAX);
            return commentIdx;
        }
    }
    return PrettyPrintWithCommentsHelper(comments, commentIdx, indent, sortKeys, depth, sb,
                                         changePositions, indentChar, true);
}

int64_t JObject::CompressedLengthAndPositionsWithinThresholds(int64_t sbLength,
                                                              int64_t maxInitialPosition,
                                                              int64_t maxSbLen) const {
    if (position >= maxInitialPosition || sbLength >= maxSbLen) return -1;
    size_t ctr = 0;
    sbLength += 1; // opening '{'
    for (auto& kv : children) {
        sbLength += static_cast<int64_t>(StrToString(kv.first, true).size()) + 2;
        sbLength = kv.second->CompressedLengthAndPositionsWithinThresholds(sbLength, maxInitialPosition, maxSbLen);
        if (sbLength == -1 || sbLength >= maxSbLen) return -1;
        sbLength += (ctr < children.size() - 1) ? 2 : 1;
        ctr++;
    }
    return sbLength;
}

bool JObject::Equals(const JNode& other) const {
    if (other.type != DT_OBJ)
        throw ComparisonError("Cannot compare object " + ToString() + " to non-object " + other.ToString());
    const JObject& othobj = static_cast<const JObject&>(other);
    if (children.size() != othobj.children.size()) return false;
    for (auto& kv : children) {
        const JNodePtr* valobj = othobj.children.find(kv.first);
        if (!valobj || !kv.second->Equals(**valobj)) return false;
    }
    return true;
}

JNodePtr JObject::Copy() const {
    auto copy = std::make_shared<JObject>(position);
    copy->children.reserve(children.size());
    for (auto& kv : children) copy->children.set(kv.first, kv.second->Copy());
    return copy;
}

void JObject::StringifyAllValuesInIniFile() {
    for (auto& kv : children) {
        JObject* section = AsObj(kv.second);
        if (!section)
            throw JsonToolsError("Only objects where all children are objects with only string values can be converted to ini files");
        for (auto& sectKv : section->children) {
            if (!sectKv.second->IsStrValue())
                sectKv.second = JNode::Str(sectKv.second->ToString(), sectKv.second->position);
        }
    }
}

std::string JObject::ToIniFile(std::vector<Comment>& comments) {
    std::string sb;
    size_t positionInComments = 0;
    for (auto& kv : children) {
        const std::string& header = kv.first;
        JObject* section = AsObj(kv.second);
        if (!section)
            throw JsonToolsError("Only objects where all children are objects with only string values can be converted to ini files");
        positionInComments = Comment::AppendAllCommentsBeforePosition(
            sb, comments, positionInComments, section->position, "", DocumentType::INI);
        sb += "[" + header + "]" + NL;
        for (auto& sectKv : section->children) {
            if (!sectKv.second->IsStrValue())
                throw JsonToolsError("Only objects where all children are objects with only string values can be converted to ini files");
            positionInComments = Comment::AppendAllCommentsBeforePosition(
                sb, comments, positionInComments, sectKv.second->position, "", DocumentType::INI);
            sb += sectKv.first + "=" + sectKv.second->GetStr() + NL;
        }
    }
    Comment::AppendAllCommentsBeforePosition(sb, comments, positionInComments, I64_MAX, "",
                                             DocumentType::INI);
    return sb;
}

// ── JArray ─────────────────────────────────────────────────────────────────
std::string JArray::ToString(bool sortKeys, const std::string& keyValueSep,
                             const std::string& itemSep, int64_t maxLength) const {
    std::string sb;
    sb.reserve(4 * Length());
    const_cast<JArray*>(this)->ToStringHelper(sortKeys, keyValueSep, itemSep, sb, false, maxLength);
    if (static_cast<int64_t>(sb.size()) >= maxLength) sb += "...";
    return sb;
}

int JArray::ToStringHelper(bool sortKeys, const std::string& keyValueSep,
                           const std::string& itemSep, std::string& sb, bool changePositions,
                           int64_t maxLength) {
    if (static_cast<int64_t>(sb.size()) >= maxLength) return -1;
    if (changePositions) position = static_cast<int64_t>(sb.size());
    sb += '[';
    size_t ctr = 0;
    for (JNodePtr& v : children) {
        if (v->ToStringHelper(sortKeys, keyValueSep, itemSep, sb, changePositions, maxLength) == -1 ||
            static_cast<int64_t>(sb.size()) >= maxLength)
            return -1;
        if (++ctr < children.size()) sb += itemSep;
    }
    sb += ']';
    return 0;
}

std::string JArray::PrettyPrint(int indent, bool sortKeys, PrettyPrintStyle style,
                                int64_t maxLength, char indentChar) {
    std::string sb;
    sb.reserve(6 * Length());
    PrettyPrintHelper(indent, sortKeys, style, 0, sb, false, maxLength, indentChar);
    if (static_cast<int64_t>(sb.size()) >= maxLength) sb += "...";
    return sb;
}

std::string JArray::ToStringAndChangePositions(bool sortKeys, const std::string& keyValueSep,
                                               const std::string& itemSep, int64_t maxLength) {
    std::string sb;
    sb.reserve(4 * Length());
    ToStringHelper(sortKeys, keyValueSep, itemSep, sb, true, maxLength);
    if (static_cast<int64_t>(sb.size()) >= maxLength) sb += "...";
    return sb;
}

std::string JArray::PrettyPrintAndChangePositions(int indent, bool sortKeys,
                                                  PrettyPrintStyle style, int64_t maxLength,
                                                  char indentChar) {
    std::string sb;
    sb.reserve(6 * Length());
    PrettyPrintHelper(indent, sortKeys, style, 0, sb, true, maxLength, indentChar);
    if (static_cast<int64_t>(sb.size()) >= maxLength) sb += "...";
    return sb;
}

int JArray::PrettyPrintHelper(int indent, bool sortKeys, PrettyPrintStyle style, int depth,
                              std::string& sb, bool changePositions, int64_t maxLength,
                              char indentChar) {
    if (static_cast<int64_t>(sb.size()) >= maxLength) return -1;
    std::string dent(static_cast<size_t>(indent * depth), indentChar);
    switch (style) {
        case PrettyPrintStyle::Whitesmith: {
            sb += dent;
            if (changePositions) position = static_cast<int64_t>(sb.size());
            sb += '[';
            sb += NL;
            size_t ctr = 0;
            for (JNodePtr& v : children) {
                if (!(v->type == DT_OBJ || v->type == DT_ARR)) sb += dent;
                if (v->PrettyPrintHelper(indent, sortKeys, style, depth + 1, sb, changePositions,
                                         maxLength, indentChar) == -1 ||
                    static_cast<int64_t>(sb.size()) >= maxLength)
                    return -1;
                if (++ctr < children.size()) sb += ',';
                sb += NL;
            }
            sb += dent;
            sb += ']';
            break;
        }
        case PrettyPrintStyle::Google: {
            if (changePositions) position = static_cast<int64_t>(sb.size());
            sb += '[';
            sb += NL;
            std::string extraDent(static_cast<size_t>((depth + 1) * indent), indentChar);
            size_t ctr = 0;
            for (JNodePtr& v : children) {
                sb += extraDent;
                if (v->PrettyPrintHelper(indent, sortKeys, style, depth + 1, sb, changePositions,
                                         maxLength, indentChar) == -1 ||
                    static_cast<int64_t>(sb.size()) >= maxLength)
                    return -1;
                if (++ctr < children.size()) sb += ',';
                sb += NL;
            }
            sb += dent;
            sb += ']';
            break;
        }
        case PrettyPrintStyle::PPrint: {
            if (changePositions) position = static_cast<int64_t>(sb.size());
            sb += '[';
            sb += NL;
            std::string extraDent(static_cast<size_t>((depth + 1) * indent), indentChar);
            size_t ctr = 0;
            for (JNodePtr& v : children) {
                int64_t maxLineEnd = static_cast<int64_t>(sb.size()) + PPRINT_LINE_LENGTH;
                sb += extraDent;
                if (v->PPrintHelper(indent, depth, sortKeys, sb, changePositions, maxLineEnd,
                                    maxLength, indentChar) == -1 ||
                    static_cast<int64_t>(sb.size()) >= maxLength)
                    return -1;
                if (++ctr < children.size()) sb += ',';
                sb += NL;
            }
            sb += dent;
            sb += ']';
            break;
        }
    }
    return 0;
}

int JArray::PPrintHelper(int indent, int depth, bool sortKeys, std::string& sb,
                         bool changePositions, int64_t maxLineEnd, int64_t maxLength,
                         char indentChar) {
    // a non-minimal-whitespace-compressed array has >= 3 chars per element
    if (static_cast<int64_t>(Length()) > PPRINT_LINE_LENGTH / 3)
        return PrettyPrintHelper(indent, sortKeys, PrettyPrintStyle::PPrint, depth + 1, sb,
                                 changePositions, maxLength, indentChar);
    size_t ogSbLen = sb.size();
    int result = ToStringHelper(sortKeys, ": ", ", ", sb, changePositions, maxLineEnd);
    if (result == -1) {
        sb.resize(ogSbLen);
        return PrettyPrintHelper(indent, sortKeys, PrettyPrintStyle::PPrint, depth + 1, sb,
                                 changePositions, maxLength, indentChar);
    }
    return result;
}

size_t JArray::PrettyPrintWithCommentsHelper(std::vector<Comment>& comments, size_t commentIdx,
                                             int indent, bool sortKeys, int depth,
                                             std::string& sb, bool changePositions,
                                             char indentChar, bool pprint) {
    if (changePositions) position = static_cast<int64_t>(sb.size());
    sb += '[';
    sb += NL;
    std::string dent(static_cast<size_t>(indent * depth), indentChar);
    std::string extraDent(static_cast<size_t>((depth + 1) * indent), indentChar);
    size_t ctr = 0;
    for (JNodePtr& v : children) {
        commentIdx = Comment::AppendAllCommentsBeforePosition(sb, comments, commentIdx,
                                                              v->position, extraDent);
        int64_t maxLineEnd = static_cast<int64_t>(sb.size()) + PPRINT_LINE_LENGTH;
        sb += extraDent;
        commentIdx = pprint
            ? v->PPrintWithCommentsHelper(comments, commentIdx, indent, sortKeys, depth + 1, sb,
                                          changePositions, indentChar, maxLineEnd)
            : v->PrettyPrintWithCommentsHelper(comments, commentIdx, indent, sortKeys, depth + 1,
                                               sb, changePositions, indentChar, false);
        if (++ctr < children.size()) sb += ',';
        sb += NL;
    }
    sb += dent;
    sb += ']';
    return commentIdx;
}

size_t JArray::PPrintWithCommentsHelper(std::vector<Comment>& comments, size_t commentIdx,
                                        int indent, bool sortKeys, int depth, std::string& sb,
                                        bool changePositions, char indentChar,
                                        int64_t maxSbLen) {
    if (static_cast<int64_t>(Length()) <= PPRINT_LINE_LENGTH / 3) {
        int64_t ogSbLen = static_cast<int64_t>(sb.size());
        int64_t nextCommentPos = commentIdx >= comments.size() ? I64_MAX : comments[commentIdx].position;
        if (CompressedLengthAndPositionsWithinThresholds(ogSbLen, nextCommentPos, maxSbLen) != -1) {
            ToStringHelper(sortKeys, ": ", ", ", sb, changePositions, I64_MAX);
            return commentIdx;
        }
    }
    return PrettyPrintWithCommentsHelper(comments, commentIdx, indent, sortKeys, depth, sb,
                                         changePositions, indentChar, true);
}

int64_t JArray::CompressedLengthAndPositionsWithinThresholds(int64_t sbLength,
                                                             int64_t maxInitialPosition,
                                                             int64_t maxSbLen) const {
    if (position >= maxInitialPosition || sbLength >= maxSbLen) return -1;
    sbLength += 1; // opening '['
    for (size_t ii = 0; ii < children.size(); ii++) {
        sbLength = children[ii]->CompressedLengthAndPositionsWithinThresholds(sbLength, maxInitialPosition, maxSbLen);
        if (sbLength == -1 || sbLength >= maxSbLen) return -1;
        sbLength += (ii < children.size() - 1) ? 2 : 1;
    }
    return sbLength;
}

bool JArray::Equals(const JNode& other) const {
    if (other.type != DT_ARR)
        throw ComparisonError("Cannot compare array " + ToString() + " to non-array " + other.ToString());
    const JArray& otharr = static_cast<const JArray&>(other);
    if (children.size() != otharr.children.size()) return false;
    for (size_t ii = 0; ii < children.size(); ii++)
        if (!children[ii]->Equals(*otharr.children[ii])) return false;
    return true;
}

JNodePtr JArray::Copy() const {
    auto copy = std::make_shared<JArray>(position);
    copy->children.reserve(children.size());
    for (const JNodePtr& child : children) copy->children.push_back(child->Copy());
    return copy;
}

std::string JArray::ToJsonLines(bool sortKeys, const std::string& keyValueSep,
                                const std::string& itemSep) {
    std::string sb;
    for (size_t ii = 0; ii < children.size(); ii++) {
        sb += children[ii]->ToString(sortKeys, keyValueSep, itemSep);
        if (ii < children.size() - 1) sb += '\n';
    }
    return sb;
}

// ── JMutator ───────────────────────────────────────────────────────────────
JNodePtr JMutator::Operate(JNodePtr inp) {
    CurJson* cjsel = dynamic_cast<CurJson*>(selector.get());
    JNodePtr selected = cjsel ? cjsel->function(inp) : selector;
    CurJson* cjmut = dynamic_cast<CurJson*>(mutator.get());
    if (cjmut) {
        auto& func = cjmut->function;
        if (JObject* xobj = AsObj(selected)) {
            for (auto& kv : xobj->children) kv.second->MutateInto(*func(kv.second));
        } else if (JArray* xarr = AsArr(selected)) {
            for (JNodePtr& v : xarr->children) v->MutateInto(*func(v));
        } else {
            selected->MutateInto(*func(selected));
        }
        return inp;
    }
    if (JObject* xobj = AsObj(selected)) {
        for (auto& kv : xobj->children) kv.second->MutateInto(*mutator);
    } else if (JArray* xarr = AsArr(selected)) {
        for (JNodePtr& v : xarr->children) v->MutateInto(*mutator);
    } else {
        selected->MutateInto(*mutator);
    }
    return inp;
}

// ── VarAssign ──────────────────────────────────────────────────────────────
VarAssign::VarAssign(std::string name, JNodePtr json, int indexOfDeclaration,
                     VariableAssignmentType assignmentType, bool isFunctionOfInput)
    : JNode(JValue{}, DT_UNKNOWN, 0),
      Name(std::move(name)),
      AssignmentType(assignmentType),
      IndexOfDeclaration(indexOfDeclaration),
      IsFunctionOfInput(isFunctionOfInput),
      varValue(std::move(json)) {
    if (assignmentType == VariableAssignmentType::INVALID)
        throw JsonToolsError("assignment type INVALID is not valid");
    OriginalValue = dynamic_cast<CurJson*>(varValue.get()) ? varValue : varValue->Copy();
}

JNodePtr VarAssign::GetInitialValueOfVariable(bool isCompiling) {
    bool valueNeedsReset = !dynamic_cast<CurJson*>(varValue.get()) && IsFunctionOfInput;
    if (valueNeedsReset && !isCompiling) varValue = OriginalValue->Copy();
    JNodePtr vaval = varValue;
    if (valueNeedsReset)
        return std::make_shared<CurJson>(vaval->type, [vaval](JNodePtr) { return vaval; });
    return vaval;
}

// ── JQueryContext ──────────────────────────────────────────────────────────
JNodePtr JQueryContext::Operate(JNodePtr inp) {
    ArgFunctionInitializeGlobals(mutatesInput_);
    JNodePtr lastStatement =
        EvaluateStatementsFromStartToEnd(inp, 0, static_cast<int64_t>(statements_.size()));
    Reset();
    return lastStatement;
}

bool JQueryContext::IsSimpleQuery() const {
    // C# indexes statements[0] unguarded and throws IndexOutOfRange on a
    // query with no statements (e.g. comments only); throw the equivalent
    if (statements_.empty()) throw JsonToolsError("Empty query");
    if (statements_.size() > 1) return false;
    return dynamic_cast<VarAssign*>(statements_[0].get()) == nullptr;
}

std::vector<std::string> JQueryContext::Varnames() const { return locals_.keys(); }

void JQueryContext::AddStatement(JNodePtr statement) {
    statements_.push_back(statement);
    indexInStatements = static_cast<int64_t>(statements_.size());
    if (auto va = std::dynamic_pointer_cast<VarAssign>(statement)) {
        if (va->AssignmentType == VariableAssignmentType::LOOP)
            loopVariableAssignmentStack_.push_back(va);
        locals_.set(va->Name, va->GetInitialValueOfVariable(true));
    } else if (dynamic_cast<LoopEnd*>(statement.get())) {
        if (loopVariableAssignmentStack_.empty())
            throw JsonToolsError("No loop variable was assigned when a LoopEnd was created");
        auto lastAssignedLoopVar = loopVariableAssignmentStack_.back();
        loopVariableAssignmentStack_.pop_back();
        lastAssignedLoopVar->IndexOfLoopEnd = indexInStatements;
    } else if (dynamic_cast<JMutator*>(statement.get())) {
        mutatesInput_ = true;
    }
}

bool JQueryContext::TryGetValue(int64_t tokenIndex, const std::string& varname, JNodePtr& val) {
    val = nullptr;
    JNodePtr* locvalP = locals_.find(varname);
    if (!locvalP) return false;
    JNodePtr locval = *locvalP;
    if (!locval) { // placeholder for undeclared variables
        int64_t indexOfDeclaration = -1;
        for (size_t i = 0; i < statements_.size(); i++) {
            if (auto va = dynamic_cast<VarAssign*>(statements_[i].get());
                va && va->Name == varname) {
                indexOfDeclaration = static_cast<int64_t>(i);
                break;
            }
        }
        throw RemesPathNameException(varname, indexInStatements, indexOfDeclaration);
    }
    if (auto cj = dynamic_cast<CurJson*>(locval.get())) {
        tokenIndicesOfVariableReferences_[tokenIndex] = varname;
        auto* self = this;
        std::string name = varname;
        val = std::make_shared<CurJson>(cj->type,
                                        [self, name](JNodePtr) { return self->cachedLocals_[name]; });
        return true;
    }
    val = locval;
    return true;
}

bool JQueryContext::AnyVariableReferencedInRange(int64_t startIndex, int64_t endIndex) const {
    for (auto& kv : tokenIndicesOfVariableReferences_)
        if (kv.first >= startIndex && kv.first < endIndex) return true;
    return false;
}

JNodePtr JQueryContext::GetQuery() {
    if (IsSimpleQuery()) {
        JNodePtr firstStatement = statements_[0];
        if (auto cj = dynamic_cast<CurJson*>(firstStatement.get())) {
            auto fun = cj->function;
            return std::make_shared<CurJson>(cj->type, [fun](JNodePtr x) {
                ArgFunctionInitializeGlobals(false);
                return fun(std::move(x));
            });
        }
        if (auto jm = dynamic_cast<JMutator*>(firstStatement.get())) {
            if (auto selector = dynamic_cast<CurJson*>(jm->selector.get())) {
                auto selectorFunc = selector->function;
                jm->selector = std::make_shared<CurJson>(selector->type, [selectorFunc](JNodePtr x) {
                    ArgFunctionInitializeGlobals(true);
                    return selectorFunc(std::move(x));
                });
            }
            return firstStatement;
        }
        return firstStatement;
    }
    Reset();
    return shared_from_this();
}

JNodePtr JQueryContext::AssignVariable(const std::shared_ptr<VarAssign>& va, JNodePtr inp) {
    JNodePtr vaval = va->GetInitialValueOfVariable(false);
    const std::string& varname = va->Name;
    locals_.set(varname, nullptr);
    JNodePtr cachedVal = nullptr;
    if (auto cj = dynamic_cast<CurJson*>(vaval.get())) {
        cachedVal = cj->function(inp);
        cachedLocals_[varname] = cachedVal;
    }
    locals_.set(varname, vaval);
    return cachedVal ? cachedVal : vaval;
}

JNodePtr JQueryContext::EvaluateForLoop(const std::shared_ptr<VarAssign>& va, JNodePtr inp) {
    auto cachedIt = cachedLocals_.find(va->Name);
    if (cachedIt == cachedLocals_.end())
        throw RemesPathNameException(va->Name, indexInStatements, va->IndexOfDeclaration);
    JNodePtr toLoopOver = cachedIt->second;
    JArray* arr = AsArr(toLoopOver);
    if (!arr) throw RemesPathLoopVarNotAnArrayException(*va, toLoopOver->type);
    int64_t statementCount = static_cast<int64_t>(statements_.size());
    int64_t endOfLoop = va->IndexOfLoopEnd > statementCount ? statementCount : va->IndexOfLoopEnd;
    int64_t startOfLoop = va->IndexOfDeclaration + 1;
    if (startOfLoop >= endOfLoop) {
        indexInStatements = endOfLoop;
        return toLoopOver;
    }
    for (JNodePtr& child : arr->children) {
        cachedLocals_[va->Name] = child;
        EvaluateStatementsFromStartToEnd(inp, startOfLoop, endOfLoop);
    }
    return toLoopOver;
}

JNodePtr JQueryContext::EvaluateStatementsFromStartToEnd(JNodePtr inp, int64_t start, int64_t end) {
    JNodePtr lastStatement = nullptr;
    indexInStatements = start;
    while (indexInStatements < end) {
        JNodePtr statement = statements_[static_cast<size_t>(indexInStatements)];
        if (auto va = std::dynamic_pointer_cast<VarAssign>(statement)) {
            lastStatement = AssignVariable(va, inp);
            if (va->AssignmentType == VariableAssignmentType::LOOP)
                EvaluateForLoop(va, inp);
            else
                indexInStatements++;
        } else if (dynamic_cast<LoopEnd*>(statement.get())) {
            indexInStatements++;
        } else {
            lastStatement = statement->CanOperate() ? statement->Operate(inp) : statement;
            indexInStatements++;
        }
    }
    regexSearchResultsShouldBeCached = true;
    return lastStatement;
}

void JQueryContext::Reset() {
    for (const std::string& varname : Varnames()) locals_.set(varname, nullptr);
    cachedLocals_.clear();
}

} // namespace jsontools
