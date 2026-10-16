// Regex.mm — NSRegularExpression (ICU) implementation of the Regex wrapper
// declared in JNode.h. The engine is UTF-8-native; NSRegularExpression is
// UTF-16-native, so every result range is mapped back to UTF-8 byte offsets.

#import <Foundation/Foundation.h>

#include "JNode.h"

namespace jsontools {

// C# sorts object keys with StringComparer.CurrentCultureIgnoreCase. The
// macOS equivalent is CFStringCompare with localized + case-insensitive
// flags (both are ICU/CLDR collation underneath): "baßk" < "basst",
// "blüe" < "blve", "oyster" < "öyster". One deliberate divergence: keys that
// differ ONLY by case compare equal in both worlds, and this port breaks the
// tie by ordinal order (uppercase first) for deterministic output, where
// .NET's Array.Sort order for such ties is unspecified.
bool KeyLessIgnoreCase(const std::string& a, const std::string& b) {
    // fast path: strings of plain ASCII alphanumerics/space order identically
    // under CLDR and ASCII casefold
    bool simple = true;
    for (char ch : a)
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') ||
              (ch >= 'A' && ch <= 'Z') || ch == ' ')) { simple = false; break; }
    if (simple)
        for (char ch : b)
            if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') ||
                  (ch >= 'A' && ch <= 'Z') || ch == ' ')) { simple = false; break; }
    if (simple) {
        size_t n = std::min(a.size(), b.size());
        for (size_t i = 0; i < n; i++) {
            unsigned char ca = static_cast<unsigned char>(a[i]);
            unsigned char cb = static_cast<unsigned char>(b[i]);
            unsigned char la = (ca >= 'A' && ca <= 'Z') ? ca + 32 : ca;
            unsigned char lb = (cb >= 'A' && cb <= 'Z') ? cb + 32 : cb;
            if (la != lb) return la < lb;
        }
        if (a.size() != b.size()) return a.size() < b.size();
        return a < b;
    }
    @autoreleasepool {
        NSString* na = [[NSString alloc] initWithBytes:a.data() length:a.size()
                                              encoding:NSUTF8StringEncoding];
        NSString* nb = [[NSString alloc] initWithBytes:b.data() length:b.size()
                                              encoding:NSUTF8StringEncoding];
        if (!na || !nb) return a < b;
        NSComparisonResult r =
            [na compare:nb
                options:NSCaseInsensitiveSearch
                  range:NSMakeRange(0, na.length)
                 locale:[NSLocale currentLocale]];
        if (r != NSOrderedSame) return r == NSOrderedAscending;
        return a < b;
    }
}

struct RegexImpl {
    NSRegularExpression* re = nil;
    std::vector<std::string> groupNames; // parallel to group numbers where known
};

// Scan an ICU pattern for named groups "(?<name>" (not lookbehind "(?<=" / "(?<!").
static std::vector<std::string> ScanGroupNames(const std::string& pattern) {
    std::vector<std::string> names;
    for (size_t i = 0; i + 2 < pattern.size(); i++) {
        if (pattern[i] == '\\') { i++; continue; }
        if (pattern[i] == '(' && pattern[i + 1] == '?' && pattern[i + 2] == '<' &&
            i + 3 < pattern.size() && pattern[i + 3] != '=' && pattern[i + 3] != '!') {
            size_t end = pattern.find('>', i + 3);
            if (end != std::string::npos) names.push_back(pattern.substr(i + 3, end - (i + 3)));
        }
    }
    return names;
}

static NSString* ToNS(const std::string& s) {
    NSString* out = [[NSString alloc] initWithBytes:s.data()
                                             length:s.size()
                                           encoding:NSUTF8StringEncoding];
    // invalid UTF-8 → lossy conversion so we never return nil
    if (!out) out = [NSString stringWithCString:s.c_str() encoding:NSISOLatin1StringEncoding];
    return out ?: @"";
}

static std::string FromNS(NSString* s) {
    if (!s) return "";
    const char* c = [s UTF8String];
    return c ? std::string(c) : std::string();
}

// Builds a map from UTF-16 index to UTF-8 byte offset for one haystack.
struct Utf16ToUtf8Map {
    std::vector<int64_t> map; // map[utf16 index] = utf8 byte offset; size = len16+1
    explicit Utf16ToUtf8Map(const std::string& utf8) {
        map.reserve(utf8.size() + 1);
        size_t i = 0, n = utf8.size();
        while (i < n) {
            unsigned char c = static_cast<unsigned char>(utf8[i]);
            size_t charLen;
            int units16;
            if (c < 0x80) { charLen = 1; units16 = 1; }
            else if ((c >> 5) == 0x6) { charLen = 2; units16 = 1; }
            else if ((c >> 4) == 0xE) { charLen = 3; units16 = 1; }
            else if ((c >> 3) == 0x1E) { charLen = 4; units16 = 2; } // surrogate pair
            else { charLen = 1; units16 = 1; } // invalid byte; treat as 1
            for (int u = 0; u < units16; u++) map.push_back(static_cast<int64_t>(i));
            i += charLen;
        }
        map.push_back(static_cast<int64_t>(n));
    }
    int64_t operator[](NSUInteger idx16) const {
        return idx16 < map.size() ? map[idx16] : (map.empty() ? 0 : map.back());
    }
};

Regex::Regex(const std::string& pattern, bool ignoreCase, bool multiline)
    : impl_(nullptr), pattern_(pattern), ignoreCase_(ignoreCase) {
    @autoreleasepool {
        NSRegularExpressionOptions opts = 0;
        if (ignoreCase) opts |= NSRegularExpressionCaseInsensitive;
        if (multiline) opts |= NSRegularExpressionAnchorsMatchLines;
        NSError* err = nil;
        NSRegularExpression* re =
            [NSRegularExpression regularExpressionWithPattern:ToNS(pattern) options:opts error:&err];
        if (!re) {
            std::string msg = "invalid regular expression: " + pattern;
            if (err) msg += std::string(" (") + FromNS(err.localizedDescription) + ")";
            throw JsonToolsError(msg);
        }
        auto* impl = new RegexImpl;
        impl->re = re;
        impl->groupNames = ScanGroupNames(pattern);
        impl_ = impl;
    }
}

Regex::~Regex() { delete static_cast<RegexImpl*>(impl_); }

int Regex::GroupCount() const {
    return static_cast<int>(static_cast<RegexImpl*>(impl_)->re.numberOfCaptureGroups);
}

std::vector<std::string> Regex::GroupNames() const {
    return static_cast<RegexImpl*>(impl_)->groupNames;
}

bool Regex::IsMatch(const std::string& s) const {
    @autoreleasepool {
        NSString* ns = ToNS(s);
        NSRegularExpression* re = static_cast<RegexImpl*>(impl_)->re;
        NSRange r = [re rangeOfFirstMatchInString:ns options:0 range:NSMakeRange(0, ns.length)];
        return r.location != NSNotFound;
    }
}

static Regex::Match BuildMatch(NSTextCheckingResult* m, NSString* ns,
                               const Utf16ToUtf8Map& map, const std::string& s) {
    Regex::Match out;
    out.success = true;
    NSRange whole = m.range;
    out.start = map[whole.location];
    out.length = map[whole.location + whole.length] - out.start;
    out.value = s.substr(static_cast<size_t>(out.start), static_cast<size_t>(out.length));
    NSUInteger n = m.numberOfRanges;
    out.groups.resize(n);
    for (NSUInteger g = 0; g < n; g++) {
        NSRange r = [m rangeAtIndex:g];
        Regex::Group& grp = out.groups[g];
        if (r.location == NSNotFound) continue;
        grp.matched = true;
        grp.start = map[r.location];
        grp.length = map[r.location + r.length] - grp.start;
        grp.value = s.substr(static_cast<size_t>(grp.start), static_cast<size_t>(grp.length));
    }
    (void)ns;
    return out;
}

Regex::Match Regex::FirstMatch(const std::string& s, int64_t startAt) const {
    @autoreleasepool {
        NSString* ns = ToNS(s);
        Utf16ToUtf8Map map(s);
        // find the utf16 index for byte offset startAt
        NSUInteger start16 = 0;
        if (startAt > 0) {
            while (start16 < map.map.size() && map.map[start16] < startAt) start16++;
        }
        NSRegularExpression* re = static_cast<RegexImpl*>(impl_)->re;
        NSTextCheckingResult* m =
            [re firstMatchInString:ns options:0 range:NSMakeRange(start16, ns.length - start16)];
        Match out;
        if (!m) return out;
        out = BuildMatch(m, ns, map, s);
        out.groupNames.resize(out.groups.size());
        auto& names = static_cast<RegexImpl*>(impl_)->groupNames;
        for (size_t i = 0; i < names.size() && i + 1 < out.groupNames.size(); i++)
            out.groupNames[i + 1] = names[i];
        return out;
    }
}

std::vector<Regex::Match> Regex::Matches(const std::string& s) const {
    @autoreleasepool {
        NSString* ns = ToNS(s);
        Utf16ToUtf8Map map(s);
        NSRegularExpression* re = static_cast<RegexImpl*>(impl_)->re;
        NSArray<NSTextCheckingResult*>* ms = [re matchesInString:ns
                                                         options:0
                                                           range:NSMakeRange(0, ns.length)];
        std::vector<Match> out;
        out.reserve(ms.count);
        auto& names = static_cast<RegexImpl*>(impl_)->groupNames;
        for (NSTextCheckingResult* m in ms) {
            Match mt = BuildMatch(m, ns, map, s);
            mt.groupNames.resize(mt.groups.size());
            for (size_t i = 0; i < names.size() && i + 1 < mt.groupNames.size(); i++)
                mt.groupNames[i + 1] = names[i];
            out.push_back(std::move(mt));
        }
        return out;
    }
}

// .NET-style replacement template: $$ → $, $0..$99, ${name}, $& (whole match).
static std::string ExpandTemplate(const std::string& repl, const Regex::Match& m) {
    std::string out;
    size_t i = 0, n = repl.size();
    while (i < n) {
        char c = repl[i];
        if (c != '$' || i + 1 >= n) { out += c; i++; continue; }
        char next = repl[i + 1];
        if (next == '$') { out += '$'; i += 2; continue; }
        if (next == '&') {
            out += m.value;
            i += 2;
            continue;
        }
        if (next == '{') {
            size_t close = repl.find('}', i + 2);
            if (close != std::string::npos) {
                std::string name = repl.substr(i + 2, close - (i + 2));
                bool allDigits = !name.empty() &&
                    name.find_first_not_of("0123456789") == std::string::npos;
                if (allDigits) {
                    size_t gi = static_cast<size_t>(std::stoll(name));
                    if (gi < m.groups.size() && m.groups[gi].matched) out += m.groups[gi].value;
                    i = close + 1;
                    continue;
                }
                for (size_t g = 1; g < m.groupNames.size(); g++) {
                    if (m.groupNames[g] == name) {
                        if (m.groups[g].matched) out += m.groups[g].value;
                        break;
                    }
                }
                i = close + 1;
                continue;
            }
        }
        if (next >= '0' && next <= '9') {
            // longest valid group number (up to 2 digits, .NET takes longest valid)
            size_t gi = static_cast<size_t>(next - '0');
            size_t consumed = 2;
            if (i + 2 < n && repl[i + 2] >= '0' && repl[i + 2] <= '9') {
                size_t gi2 = gi * 10 + static_cast<size_t>(repl[i + 2] - '0');
                if (gi2 < m.groups.size()) { gi = gi2; consumed = 3; }
            }
            if (gi < m.groups.size()) {
                if (m.groups[gi].matched) out += m.groups[gi].value;
                i += consumed;
                continue;
            }
        }
        out += c;
        i++;
    }
    return out;
}

std::string Regex::Replace(const std::string& s, const std::string& repl) const {
    return Replace(s, [&repl](const Match& m) { return ExpandTemplate(repl, m); });
}

std::string Regex::Replace(const std::string& s,
                           const std::function<std::string(const Match&)>& fn) const {
    std::vector<Match> ms = Matches(s);
    if (ms.empty()) return s;
    std::string out;
    out.reserve(s.size());
    int64_t last = 0;
    for (const Match& m : ms) {
        out.append(s, static_cast<size_t>(last), static_cast<size_t>(m.start - last));
        out += fn(m);
        last = m.start + m.length;
    }
    out.append(s, static_cast<size_t>(last), std::string::npos);
    return out;
}

} // namespace jsontools
