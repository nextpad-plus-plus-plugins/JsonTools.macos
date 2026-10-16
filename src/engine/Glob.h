// Glob.h — port of Utils/Glob.cs.
//
// PORT DIVERGENCE: upstream treats '\\' as the path separator (Windows);
// this port uses '/' (macOS). In glob input, both '/' and '\\' denote a path
// separator; '*' and '?' refuse to cross '/' instead of '\\'.

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "JNode.h" // Regex

namespace jsontools {

enum class Ternary { FALSE_, TRUE_, UNDECIDED };

class GlobFunction {
public:
    std::function<bool(const std::string&)> matcher;
    bool orWithPrevious;
    bool negated;
    // does the pattern contain any characters in the set "*?[{"? If true, it
    // is treated as using glob syntax.
    bool usesMetacharacters;

    GlobFunction(std::function<bool(const std::string&)> matcher_, bool or_, bool negated_,
                 bool usesMetacharacters_)
        : matcher(std::move(matcher_)), orWithPrevious(or_), negated(negated_),
          usesMetacharacters(usesMetacharacters_) {}

    Ternary IsMatch(const std::string& inp, Ternary previousResult) const {
        if (orWithPrevious && previousResult == Ternary::TRUE_) return Ternary::TRUE_;
        if (!orWithPrevious && previousResult == Ternary::FALSE_) return Ternary::FALSE_;
        bool result = matcher(inp);
        return (result != negated) ? Ternary::TRUE_ : Ternary::FALSE_;
    }
};

// parser that parses glob syntax in space-separated globs (see Glob.cs
// docstring; the only change is the path separator noted above).
class Glob {
public:
    int64_t ii = 0;
    std::string errorMsg; // empty = no error
    int64_t errorPos = -1;
    std::vector<std::string> globs;
    std::vector<GlobFunction> globFunctions;

    Glob() { Reset(); }

    void Reset() {
        globs.clear();
        globFunctions.clear();
        errorMsg.clear();
        errorPos = -1;
        ii = 0;
    }

    char Peek(const std::string& inp) const {
        return ii < static_cast<int64_t>(inp.size()) - 1 ? inp[static_cast<size_t>(ii + 1)]
                                                         : '\x00';
    }

    // Converts a string following glob syntax into a regex that applies the
    // same tests. Returns nullptr on a bad pattern (errorMsg/errorPos set).
    std::pair<RegexPtr, bool> Glob2Regex(const std::string& inp);

    // Parses newline-separated simple globs; returns func(x) -> true if any
    // glob matches x.  Throws JsonToolsError on an invalid path character.
    std::function<bool(const std::string&)> ParseLinesSimple(const std::string& inp);

    std::function<bool(const std::string&)> Parse(const std::string& inp,
                                                  bool fromBeginning = true);

    static std::function<bool(const std::string&)>
    CacheGlobFuncResultsForTopDirectory(const std::string& topDir,
                                        const std::vector<GlobFunction>& globFuncs);

private:
    static std::function<bool(const std::string&)>
    ApplyAllGlobFunctions(const std::vector<GlobFunction>& globFuncs);
};

} // namespace jsontools
