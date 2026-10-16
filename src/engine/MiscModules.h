// MiscModules.h — ports of the smaller upstream engine modules:
// YamlDumper.cs, Dson.cs, IniFileParser.cs, CsvSniffer.cs.

#pragma once

#include "JNode.h"
#include "JsonParser.h"

namespace jsontools {

// ── YamlDumper ──────────────────────────────────────────────────────────────
class YamlDumper {
public:
    std::string Dump(const JNodePtr& json, int indent = 4);

private:
    void BuildYaml(std::string& sb, const JNodePtr& tok, int depth, int indent);
    std::string YamlKeyRepr(const std::string& k);
    std::string YamlValRepr(const JNodePtr& v);
};

// ── Dson (wow such data) ────────────────────────────────────────────────────
struct DsonDumpException : JsonToolsError {
    explicit DsonDumpException(const std::string& m)
        : JsonToolsError("DSON dump error: " + m) {}
};

class Dson {
public:
    static std::string Dump(const JNodePtr& json);
};

// ── IniFileParser ───────────────────────────────────────────────────────────
struct IniParserException : JsonToolsError {
    char curChar;
    int64_t position;
    int64_t lineNum; // 1-based
    IniParserException(const std::string& message, char c, int64_t pos, int64_t lineNum0Based)
        : JsonToolsError(message + " at line " + std::to_string(lineNum0Based + 1) +
                         ", position " + std::to_string(pos) + " (char " +
                         JsonLint::CharDisplay(c) + ")"),
          curChar(c), position(pos), lineNum(lineNum0Based + 1) {}
    JsonLint ToJsonLint() const {
        return JsonLint(JsonLintType::FATAL_UNSPECIFIED_ERROR, position, curChar,
                        std::string(what()));
    }
};

class IniFileParser {
public:
    int64_t ii = 0;
    int64_t lineNum = 0;
    std::vector<Comment> comments;

    void Reset() {
        ii = 0;
        lineNum = 0;
        comments.clear();
    }
    std::shared_ptr<JObject> Parse(const std::string& inp);

private:
    std::shared_ptr<JObject> ParseSection(const std::string& inp, const std::string& header);
    std::pair<std::string, JNodePtr> ParseKeyValuePair(const std::string& inp, int64_t keyIndent);
    int64_t ConsumeInsignificantChars(const std::string& inp);
    void ConsumeLine(const std::string& inp);
    int64_t ConsumeToIndent(const std::string& inp);
};

// ── CsvSniffer ──────────────────────────────────────────────────────────────
enum class EndOfLine { CRLF, CR, LF };

class CsvSniffer {
public:
    static constexpr int DEFAULT_MAX_CHARS_TO_SNIFF = 1600;
    // returns the number of columns, or -1 if text doesn't look like a
    // delimiter/eol-combo CSV
    static int64_t Sniff(const std::string& text, EndOfLine eol, char delimiter, char quote,
                         int64_t maxLinesToSniff = 16,
                         int64_t maxCharsToSniff = DEFAULT_MAX_CHARS_TO_SNIFF,
                         int64_t minLinesToDecide = 6);
};

} // namespace jsontools
