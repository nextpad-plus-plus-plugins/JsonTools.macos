// Glob.cpp — see Glob.h.

#include "Glob.h"

#include <algorithm>

namespace jsontools {

// regex metacharacters that need escaping when matched literally
static std::string EscapeRegexChar(char c) {
    static const std::string special = "\\^$.|?*+()[]{}";
    if (c == '\t') return "\\t";
    if (special.find(c) != std::string::npos) return std::string("\\") + c;
    return std::string(1, c);
}

std::pair<RegexPtr, bool> Glob::Glob2Regex(const std::string& inp) {
    std::string sb;
    int64_t start = ii;
    bool isCharClass = false;
    bool usesMetacharacters = false;
    bool isAlternation = false;
    int64_t len = static_cast<int64_t>(inp.size());
    while (ii < len) {
        char c = inp[static_cast<size_t>(ii)];
        char nextC;
        bool exitLoop = false;
        switch (c) {
        case '/':
            sb += "/"; // path separator (upstream: "\\\\" for Windows '\')
            break;
        case '\\':
            nextC = Peek(inp);
            if (nextC == 'x' || nextC == 'u' // \xNN, \uNNNN unicode escapes
                || (nextC == ']' && isCharClass))
                sb += '\\';
            else
                sb += "/"; // literal backslash upstream = Windows path separator
            break;
        case '*':
            usesMetacharacters = true;
            if (isCharClass) {
                sb += "\\*"; // "[*]" matches literal * character
                break;
            } else if (sb.empty()) {
                // since globs are only anchored at the end, leading * should
                // not influence matching ("*foo.txt" == "foo.txt")
                break;
            }
            nextC = Peek(inp);
            if (nextC == '*') {
                ii++;
                sb += ".*"; // "**" means recursive search; ignores path delimiters
            } else {
                sb += "[^/]*"; // "*" means anything but a path delimiter
            }
            break;
        case '[':
            usesMetacharacters = true;
            if (isCharClass) {
                // .NET treats '[' inside a class as a literal; ICU would start a
                // nested set, so escape it
                sb += "\\[";
                break;
            }
            sb += '[';
            nextC = Peek(inp);
            if (nextC == '!') {
                sb += "^/"; // [! begins a negated char class, but also exclude path sep
                ii++;
            }
            isCharClass = true;
            break;
        case ']':
            isCharClass = false;
            sb += ']';
            break;
        case '{':
            if (isCharClass) {
                sb += "\\{";
            } else {
                usesMetacharacters = true;
                isAlternation = true; // e.g. *.{cpp,h,c} matches *.cpp or *.h or *.c
                sb += "(?:";
            }
            break;
        case '}':
            if (isCharClass) {
                sb += "\\}";
            } else {
                sb += ")";
                isAlternation = false;
            }
            break;
        case ',':
            sb += isAlternation ? '|' : ',';
            break;
        case '.':
        case '$':
        case '(':
        case ')':
        case '^':
            // no special meaning in glob syntax, but regex metacharacters
            sb += '\\';
            sb += c;
            break;
        case '?':
            if (isCharClass) {
                sb += '?';
            } else {
                usesMetacharacters = true;
                sb += "[^/]"; // '?' is any single char but a path delimiter
            }
            break;
        case '|':
        case '<':
        case '>':
        case ';':
        case '\t':
        case '\r':
        case '\n':
            exitLoop = true; // glob-syntax separators, never part of a pattern
            break;
        case ' ':
        case '!':
            if (isCharClass)
                sb += c;
            else
                exitLoop = true; // special chars outside char classes
            break;
        default:
            sb += c;
            break;
        }
        if (exitLoop) break;
        ii++;
    }
    if (usesMetacharacters) // anything without metacharacters is a normal substring test
        sb += '$'; // globs are anchored at the end: "*foo" does not match "foo/bar.txt"
    try {
        auto regex = std::make_shared<Regex>(sb, /*ignoreCase=*/true, /*multiline=*/false);
        globs.push_back(sb);
        return {regex, usesMetacharacters};
    } catch (const std::exception& ex) {
        errorMsg = ex.what();
        errorPos = start;
        return {nullptr, false};
    }
}

std::function<bool(const std::string&)> Glob::ParseLinesSimple(const std::string& inp) {
    std::string sb = "(?:";
    Reset();
    int64_t len = static_cast<int64_t>(inp.size());
    while (ii < len) {
        char c = inp[static_cast<size_t>(ii)];
        char nextC;
        switch (c) {
        case '\r':
            nextC = Peek(inp);
            if (nextC == '\n') ii++;
            // end of the current pattern; the next pattern must also match the whole string
            sb += '|';
            break;
        case '\n':
            sb += '|';
            break;
        case '*':
            nextC = Peek(inp);
            if (nextC == '*') {
                ii++;
                sb += ".*"; // "**" means recursive search; ignores path delimiters
            } else {
                sb += "[^/]*"; // "*" means anything but a path delimiter
            }
            break;
        case '\\':
        case '/':
            sb += "/";
            break;
        case '?':
            sb += "[^/]"; // '?' is any single char other than a path delimiter
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20 || c == '\x7f')
                throw JsonToolsError(std::string("Filesystem paths cannot contain the character '") +
                                     c + "'");
            sb += EscapeRegexChar(c);
            break;
        }
        ii++;
    }
    sb += ")$";
    auto rex = std::make_shared<Regex>(sb, /*ignoreCase=*/true, /*multiline=*/false);
    return [rex](const std::string& x) { return rex->IsMatch(x); };
}

std::function<bool(const std::string&)> Glob::Parse(const std::string& inp, bool fromBeginning) {
    if (fromBeginning) Reset();
    bool orNext = false;
    bool negated = false;
    std::vector<GlobFunction> localFuncs;
    std::vector<GlobFunction>& globFuncs = fromBeginning ? globFunctions : localFuncs;
    int64_t len = static_cast<int64_t>(inp.size());
    while (ii < len) {
        char c = inp[static_cast<size_t>(ii)];
        if (c == ' ' || c == '\t' || c == ';' || c == '\r' || c == '\n') {
            // skip (upstream's unused-in-production Parse leaves '\r'/'\n' to
            // Glob2Regex; its own tests expect them to separate patterns)
        } else if (c == '|') {
            orNext = true;
        } else if (c == '!') {
            negated = !negated;
        } else if (c == '<') {
            ii++;
            auto subFunc = Parse(inp, false);
            globFuncs.emplace_back(subFunc, orNext, negated, true);
            negated = false;
            orNext = false;
        } else if (c == '>') {
            break;
        } else {
            auto [globRegex, usesMetacharacters] = Glob2Regex(inp);
            if (!globRegex) continue; // ignore errors, try to parse everything else
            RegexPtr rex = globRegex;
            globFuncs.emplace_back([rex](const std::string& x) { return rex->IsMatch(x); },
                                   orNext, negated, usesMetacharacters);
            ii--;
            negated = false;
            orNext = false;
        }
        ii++;
    }
    ii++;
    return ApplyAllGlobFunctions(globFuncs);
}

std::function<bool(const std::string&)>
Glob::ApplyAllGlobFunctions(const std::vector<GlobFunction>& globFuncs) {
    if (globFuncs.empty()) return [](const std::string&) { return true; };
    if (globFuncs.size() == 1) { // special-case this for efficiency
        GlobFunction glofu = globFuncs[0];
        if (glofu.negated) return [glofu](const std::string& x) { return !glofu.matcher(x); };
        return glofu.matcher;
    }
    bool anyOr = std::any_of(globFuncs.begin(), globFuncs.end(),
                             [](const GlobFunction& gf) { return gf.orWithPrevious; });
    std::vector<GlobFunction> funcs = globFuncs; // own a copy in the closure
    if (!anyOr) {
        // more efficient short-circuiting function when no logical or is used
        return [funcs](const std::string& x) {
            for (const GlobFunction& gf : funcs)
                if (gf.matcher(x) == gf.negated) return false;
            return true;
        };
    }
    return [funcs](const std::string& x) {
        Ternary result = Ternary::UNDECIDED;
        for (const GlobFunction& globFunc : funcs) result = globFunc.IsMatch(x, result);
        return result != Ternary::FALSE_;
    };
}

std::function<bool(const std::string&)>
Glob::CacheGlobFuncResultsForTopDirectory(const std::string& topDir,
                                          const std::vector<GlobFunction>& globFuncs) {
    std::vector<GlobFunction> newFuncs;
    newFuncs.reserve(globFuncs.size());
    for (const GlobFunction& glofu : globFuncs) {
        if (!glofu.usesMetacharacters && glofu.matcher(topDir)) {
            newFuncs.emplace_back([](const std::string&) { return true; }, glofu.orWithPrevious,
                                  glofu.negated, glofu.usesMetacharacters);
        } else {
            newFuncs.push_back(glofu);
        }
    }
    return ApplyAllGlobFunctions(newFuncs);
}

} // namespace jsontools
