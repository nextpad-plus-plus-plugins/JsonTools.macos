// RemesPathLexer.cpp — port of RemesPathLexer.cs. Tokenizes a RemesPath query.

#include "RemesPath.h"

#include <cerrno>
#include <cstdlib>

namespace jsontools {

const char* RemesPathLexer::LOOP_END_KEYWORD = "end";

const std::unordered_map<std::string, VariableAssignmentType>&
RemesPathLexer::VAR_ASSIGN_KEYWORDS_TO_TYPES() {
    static auto* m = new std::unordered_map<std::string, VariableAssignmentType>{
        {"for", VariableAssignmentType::LOOP},
        {"var", VariableAssignmentType::NORMAL},
    };
    return *m;
}

// Same alternation as the C# TOKEN_REGEX; group numbers must line up with the
// switch below (ICU numbers groups by open-paren order, same as .NET).
static const char* UNQUOTED_START_RE =
    "(?:[_\\$\\p{Lu}\\p{Ll}\\p{Lt}\\p{Lm}\\p{Lo}\\p{Nl}]|\\\\u[\\da-f]{4})";

static Regex& TokenRegex() {
    static Regex* re = [] {
        std::string pattern =
            std::string("(@)|") +                                     // 1: CurJson
            "(0x[\\da-fA-F]+)|" +                                     // 2: hex numbers
            "(0|[1-9]\\d*)(\\.\\d*)?([eE][-+]?\\d+)?|" +              // 3,4,5: numbers
            "(\\.\\d+(?:[eE][-+]?\\d+)?)|" +                          // 6: leading-decimal numbers
            "(->)|" +                                                 // 7: map operator
            "(&|\\||\\^|=~|[!=]=|<=?|>=?|\\+|-|//?|%|\\*\\*?)|" +     // 8: binops
            "([,\\[\\]\\(\\)\\{\\}\\.:=!;])|" +                       // 9: delimiters
            "([gjf]?(?<!\\\\)`(?:\\\\\\\\|\\\\`|[^`\\r\\n])*`)|" +    // 10: backtick strings
            "(" + UNQUOTED_START_RE + "(?:[\\p{Mn}\\p{Mc}\\p{Nd}\\p{Pc}\\u200c\\u200d]|" +
            UNQUOTED_START_RE + ")*)|" +                              // 11: unquoted strings
            "\\#([^\\n]*)(?:\\n|\\z)|" +                              // 12: comments
            "(\\S+)";                                                 // 13: anything else (error)
        return new Regex(pattern, false, false);
    }();
    return *re;
}

Token RemesPathLexer::ParseUnquotedString(const std::string& unquoted, int64_t sourceIndex) {
    // resolve \uXXXX escapes like JsonParser.ParseUnquotedKeyHelper, using the
    // lexer's LENIENT parser (the strict default would throw on control chars)
    std::string uqs = unquoted;
    if (uqs.find("\\u") != std::string::npos) {
        int64_t savedII = jsonParser.ii;
        ParserState savedState = jsonParser.state;
        jsonParser.ii = 0;
        JNodePtr parsed = jsonParser.ParseString("\"" + unquoted + "\"");
        jsonParser.ii = savedII;
        jsonParser.state = savedState;
        if (parsed->IsStrValue()) uqs = parsed->GetStr();
    }
    if (uqs == "null") return Token::Node(JNode::Null(), sourceIndex);
    if (uqs == "NaN") return Token::Node(JNode::Float(std::numeric_limits<double>::quiet_NaN()), sourceIndex);
    if (uqs == "Infinity") return Token::Node(JNode::Float(std::numeric_limits<double>::infinity()), sourceIndex);
    if (uqs == "true") return Token::Node(JNode::Bool(true), sourceIndex);
    if (uqs == "false") return Token::Node(JNode::Bool(false), sourceIndex);
    auto bit = Binop::BINOPS().find(uqs);
    if (bit != Binop::BINOPS().end()) return Token::Bop(bit->second, sourceIndex);
    auto uit = UnaryOp::UNARY_OPS().find(uqs);
    if (uit != UnaryOp::UNARY_OPS().end()) return Token::Unop(uit->second, sourceIndex);
    return Token::Uqs(uqs, sourceIndex);
}

void RemesPathLexer::BraceMatchCheck(const std::string& q, const std::vector<Token>& toks,
                                     char open, char close) {
    int64_t lastUnclosed = -1;
    int unclosedCount = 0;
    for (const Token& tok : toks) {
        if (tok.kind == Token::DELIM) {
            if (tok.delim == open) {
                if (++unclosedCount >= MAX_RECURSION_DEPTH)
                    throw RemesLexerException(tok.sourceIndex, q,
                                              "Maximum recursion depth (" +
                                                  std::to_string(MAX_RECURSION_DEPTH) +
                                                  ") in a RemesPath query reached");
                else if (unclosedCount == 1)
                    lastUnclosed = tok.sourceIndex;
            } else if (tok.delim == close && --unclosedCount < 0) {
                throw RemesLexerException(tok.sourceIndex, q,
                                          std::string("Unmatched '") + close + "'");
            }
        }
    }
    if (unclosedCount > 0)
        throw RemesLexerException(lastUnclosed, q, std::string("Unclosed '") + open + "'");
}

std::string RemesPathLexer::StringToBacktickString(const std::string& s) {
    std::string sb = "`";
    for (char c : s) {
        switch (c) {
            case '`': sb += "\\`"; break;
            case '\\': sb += "\\\\"; break;
            case '\n': sb += "\\n"; break;
            case '\t': sb += "\\t"; break;
            case '\r': sb += "\\r"; break;
            default: sb += c; break;
        }
    }
    sb += '`';
    return sb;
}

std::vector<Token> RemesPathLexer::Tokenize(const std::string& q) {
    std::vector<Regex::Match> regtoks = TokenRegex().Matches(q);
    if (regtoks.empty()) throw RemesLexerException("Empty query");
    std::vector<Token> toks;
    toks.reserve(regtoks.size());
    for (const Regex::Match& m : regtoks) {
        size_t successfulGroup = 1;
        for (; successfulGroup < m.groups.size() && !m.groups[successfulGroup].matched;
             successfulGroup++) {}
        int64_t idx = m.start;
        switch (successfulGroup) {
            case 1:
                toks.push_back(Token::Node(std::make_shared<CurJson>(), idx));
                break;
            case 2: { // hex number
                errno = 0;
                long long hexnum = strtoll(m.value.c_str() + 2, nullptr, 16);
                if (errno == ERANGE)
                    throw RemesLexerException(idx, q, "Hex number too large for a 64-bit signed integer");
                toks.push_back(Token::Node(JNode::Int(hexnum), idx));
                break;
            }
            case 3: { // number
                if (!(m.groups[4].matched || m.groups[5].matched)) {
                    errno = 0;
                    char* endp = nullptr;
                    long long l = strtoll(m.value.c_str(), &endp, 10);
                    if (errno == ERANGE)
                        toks.push_back(Token::Node(JNode::Float(strtod(m.value.c_str(), nullptr)), idx));
                    else
                        toks.push_back(Token::Node(JNode::Int(l), idx));
                } else {
                    toks.push_back(Token::Node(JNode::Float(strtod(m.value.c_str(), nullptr)), idx));
                }
                break;
            }
            case 6:
                toks.push_back(Token::Node(JNode::Float(strtod(m.value.c_str(), nullptr)), idx));
                break;
            case 7: // "->" map operator
                toks.push_back(Token::Delim('>', idx));
                break;
            case 8:
                toks.push_back(Token::Bop(Binop::BINOPS().at(m.value), idx));
                break;
            case 9:
                toks.push_back(Token::Delim(m.value[0], idx));
                break;
            case 10: { // backtick strings
                char starter = m.value[0];
                std::string sb;
                bool escaped = false;
                size_t startIdx = starter == '`' ? 1 : 2;
                for (size_t ii = startIdx; ii + 1 < m.value.size() + 1 && ii < m.value.size() - 1; ii++) {
                    char c = m.value[ii];
                    if (c == '\\') {
                        if (escaped) sb += '\\';
                        escaped = !escaped;
                    } else if (c == '`') {
                        if (!escaped) break;
                        sb += '`';
                        escaped = false;
                    } else if (escaped) {
                        if (c == 'r') sb += '\r';
                        else if (c == 'n') sb += '\n';
                        else if (c == 't') sb += '\t';
                        else {
                            sb += '\\';
                            sb += c;
                        }
                        escaped = false;
                    } else {
                        sb += c;
                    }
                }
                if (starter == 'j') { // JSON literal
                    toks.push_back(Token::Node(jsonParser.Parse(sb), idx));
                } else if (starter == 'g') { // regex
                    toks.push_back(Token::Node(
                        std::make_shared<JRegex>(std::make_shared<Regex>(sb, false, true)), idx));
                } else if (starter == 'f') { // f-string
                    TokenizeFString(toks, sb, q, idx + 2);
                } else {
                    toks.push_back(Token::Node(JNode::Str(sb), idx));
                }
                break;
            }
            case 11:
                toks.push_back(ParseUnquotedString(m.value, idx));
                break;
            case 12: // comments are dropped
                break;
            default:
                throw RemesLexerException(idx, q, "Invalid token \"" + m.value + "\"");
        }
    }
    BraceMatchCheck(q, toks, '(', ')');
    BraceMatchCheck(q, toks, '[', ']');
    BraceMatchCheck(q, toks, '{', '}');
    return toks;
}

void RemesPathLexer::TokenizeFString(std::vector<Token>& tokens, const std::string& fstring,
                                     const std::string& query, int64_t startIndex) {
    size_t len = fstring.size();
    // interpreted as a call to s_cat, concatenating string representations
    tokens.push_back(Token::Uqs("s_cat", startIndex));
    tokens.push_back(Token::Delim('(', startIndex));
    size_t lastUnmatchedOpenCurly = 0;
    bool insideCurlyBraces = false;
    std::string sb;
    for (size_t ii = 0; ii < len; ii++) {
        char c = fstring[ii];
        switch (c) {
            case '{':
                if (ii == len - 1) {
                    if (!insideCurlyBraces) lastUnmatchedOpenCurly = ii;
                    throw RemesLexerException(startIndex + static_cast<int64_t>(lastUnmatchedOpenCurly),
                                              query, "unmatched '{' in f-string");
                }
                if (fstring[ii + 1] == '{') {
                    sb += c; // {{ matching literal {
                    ii++;
                } else {
                    if (insideCurlyBraces)
                        throw RemesLexerException(startIndex + static_cast<int64_t>(lastUnmatchedOpenCurly),
                                                  query, "unmatched '{' in f-string");
                    lastUnmatchedOpenCurly = ii;
                    insideCurlyBraces = true;
                    if (!sb.empty()) {
                        tokens.push_back(Token::Node(JNode::Str(sb)));
                        tokens.push_back(Token::Delim(','));
                        sb.clear();
                    }
                }
                break;
            case '}':
                if (insideCurlyBraces) {
                    insideCurlyBraces = false;
                    std::vector<Token> interpolatedTokens = Tokenize(sb);
                    for (Token& t : interpolatedTokens) tokens.push_back(std::move(t));
                    sb.clear();
                    if (ii < len - 1) tokens.push_back(Token::Delim(','));
                } else if (ii < len - 1 && fstring[ii + 1] == '}') {
                    sb += c; // }} representing literal }
                    ii++;
                } else {
                    throw RemesLexerException(
                        startIndex + static_cast<int64_t>(ii), query,
                        "'{' characters are not allowed in f-strings except to close an interpolated section or in the \"}}\" sequence that represents a literal '}' character.");
                }
                break;
            case '=':
                if (insideCurlyBraces)
                    throw RemesLexerException(startIndex + static_cast<int64_t>(ii), query,
                                              "'=' tokens (signifying a mutation expression) are not allowed inside f-string interpolated sections");
                sb += c;
                break;
            case ';':
                if (insideCurlyBraces)
                    throw RemesLexerException(startIndex + static_cast<int64_t>(ii), query,
                                              "';' tokens (signifying the end of a statement) are not allowed inside f-string interpolated sections");
                sb += c;
                break;
            default:
                sb += c;
                break;
        }
    }
    if (insideCurlyBraces)
        throw RemesLexerException(startIndex + static_cast<int64_t>(lastUnmatchedOpenCurly), query,
                                  "unmatched '{' in f-string");
    if (!sb.empty()) tokens.push_back(Token::Node(JNode::Str(sb)));
    tokens.push_back(Token::Delim(')'));
}

} // namespace jsontools
