// RemesPathFunctions.cpp — Binops, UnaryOps and the ArgFunction library.
// Port of RemesPathFunctions.cs (see RemesPath.h for port notes).

#include "RemesPath.h"
#include "JsonTabularize.h"
#include "JsonSchemaMaker.h"
#include "RandomJsonFromSchema.h"

#include <algorithm>
#include <cfenv>
#include <cmath>
#include <random>

namespace jsontools {

char csvDelimiterInLastQuery = '\x00';
char csvQuoteCharInLastQuery = '\x00';

int64_t& LoopCountValue() {
    static int64_t v = -1;
    return v;
}

static std::mt19937_64& Rng() {
    static std::mt19937_64 rng{std::random_device{}()};
    return rng;
}
uint64_t RandomNext(uint64_t exclusiveUpperBound) {
    if (exclusiveUpperBound == 0) return 0;
    return std::uniform_int_distribution<uint64_t>(0, exclusiveUpperBound - 1)(Rng());
}
double RandomNextDouble() {
    return std::uniform_real_distribution<double>(0.0, 1.0)(Rng());
}

// ── exceptions ─────────────────────────────────────────────────────────────
std::string RemesLexerException::MarkLocationOfSyntaxError(const std::string& query,
                                                           int64_t lexpos,
                                                           const std::string& msg) {
    if (query.empty()) return "Error: query is empty or contains only whitespace";
    if (lexpos < 0) lexpos = 0;
    else if (lexpos >= static_cast<int64_t>(query.size())) lexpos = static_cast<int64_t>(query.size()) - 1;
    return "Syntax error at position " + std::to_string(lexpos) + ": " + msg + "\r\n" +
           query.substr(0, static_cast<size_t>(lexpos)) + ">>>HERE>>>" +
           query.substr(static_cast<size_t>(lexpos));
}

static std::string ArgExcMessage(const std::string& description, int64_t argNum,
                                 const ArgFunction* func, Dtype gotType) {
    std::string validTypes = FormatDtype(func->TypeOptions(argNum));
    if (description.empty())
        return "For argument " + std::to_string(argNum) + " of function " + func->name +
               ", expected " + validTypes + ", instead got type " + FormatDtype(gotType);
    return "For argument " + std::to_string(argNum) + " of function " + func->name +
           ", expected " + validTypes + ", instead " + description;
}

RemesPathArgumentException::RemesPathArgumentException(const char* description, int64_t argNum,
                                                       const ArgFunction* func, Dtype gotType)
    : RemesPathException(ArgExcMessage(description ? description : "", argNum, func, gotType)) {}
RemesPathArgumentException::RemesPathArgumentException(const std::string& description,
                                                       int64_t argNum, const ArgFunction* func,
                                                       Dtype gotType)
    : RemesPathException(ArgExcMessage(description, argNum, func, gotType)) {}

// ── slicing (ArrayExtensions.cs) ───────────────────────────────────────────
int64_t ClampWithinLen(int64_t len, int64_t num, bool isStartIdx) {
    if (num >= len) return isStartIdx ? len - 1 : len;
    if (num < 0) {
        num += len;
        if (num < 0) return isStartIdx ? 0 : -1;
        return num;
    }
    return num;
}

// normalize a 1-3 element slicer into (start, stop, stride) like C# LazySlice
static bool NormalizeSlicer(const Slicer& slicer, bool& hasStart, int64_t& start, bool& hasStop,
                            int64_t& stop, bool& hasStride, int64_t& stride) {
    hasStart = hasStop = hasStride = false;
    start = stop = 0;
    stride = 1;
    switch (slicer.size()) {
        case 1:
            hasStart = true; start = 0;
            hasStop = slicer[0].first; stop = slicer[0].second;
            hasStride = true; stride = 1;
            // C#: {stop} means start=0, stop=slicer[0], stride=1; a null single
            // element means stop is null
            break;
        case 2:
            hasStart = slicer[0].first; start = slicer[0].second;
            hasStop = slicer[1].first; stop = slicer[1].second;
            hasStride = true; stride = 1;
            break;
        case 3:
            hasStart = slicer[0].first; start = slicer[0].second;
            hasStop = slicer[1].first; stop = slicer[1].second;
            hasStride = slicer[2].first; stride = slicer[2].second;
            break;
        default:
            return false;
    }
    return true;
}

void ResolveSlice(int64_t len, const Slicer& slicer, std::vector<int64_t>& indicesOut) {
    if (len <= 0) return;
    bool hasStart, hasStop, hasStride;
    int64_t start, stop, stride;
    if (!NormalizeSlicer(slicer, hasStart, start, hasStop, stop, hasStride, stride)) return;
    int64_t istart, istop, istride;
    if (hasStride) {
        istride = stride;
        if (istride < 0) {
            if (hasStart) {
                if (start < -len) return;
                istart = ClampWithinLen(len, start, true);
            } else {
                istart = len - 1;
            }
            istop = hasStop ? ClampWithinLen(len, stop, false) : -1;
        } else if (istride == 0) {
            throw RemesPathException(
                "The stride parameter of a slice must be a non-zero integer, or must be left empty");
        } else {
            if (hasStart) {
                if (start >= len) return;
                istart = ClampWithinLen(len, start, true);
            } else {
                istart = 0;
            }
            istop = hasStop ? ClampWithinLen(len, stop, false) : len;
        }
    } else {
        istride = 1;
        istart = hasStart ? ClampWithinLen(len, start, true) : 0;
        istop = hasStop ? ClampWithinLen(len, stop, false) : len;
    }
    if (istride < 0) {
        for (int64_t ii = istart; ii > istop; ii += istride) indicesOut.push_back(ii);
    } else {
        for (int64_t ii = istart; ii < istop; ii += istride) indicesOut.push_back(ii);
    }
}

bool WrappedIndex(int64_t len, int64_t idx, int64_t& resolved) {
    if (idx >= len || idx < -len) return false;
    resolved = idx >= 0 ? idx : idx + len;
    return true;
}

// split a UTF-8 string into per-code-point chunks
static std::vector<std::string> CodePointChunks(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        size_t len;
        DecodeCodePoint(s, i, len);
        out.push_back(s.substr(i, len));
        i += len;
    }
    return out;
}

int64_t StrLenCodePoints(const std::string& s) {
    int64_t n = 0;
    size_t i = 0;
    while (i < s.size()) {
        size_t len;
        DecodeCodePoint(s, i, len);
        i += len;
        n++;
    }
    return n;
}

std::string StrSlice(const std::string& s, const Slicer& slicer) {
    std::vector<std::string> chunks = CodePointChunks(s);
    std::vector<int64_t> indices;
    ResolveSlice(static_cast<int64_t>(chunks.size()), slicer, indices);
    std::string out;
    for (int64_t ii : indices) out += chunks[static_cast<size_t>(ii)];
    return out;
}

bool StrWrappedIndex(const std::string& s, int64_t idx, std::string& atIdx) {
    std::vector<std::string> chunks = CodePointChunks(s);
    int64_t resolved;
    if (!WrappedIndex(static_cast<int64_t>(chunks.size()), idx, resolved)) return false;
    atIdx = chunks[static_cast<size_t>(resolved)];
    return true;
}

// ── numeric coercion helpers (C# Convert.To*) ──────────────────────────────
static int64_t ToInt64(const JNodePtr& n) {
    switch (n->type) {
        case DT_BOOL: return n->GetBool() ? 1 : 0;
        case DT_INT: return n->GetInt();
        case DT_FLOAT: {
            // C# Convert.ToInt64 rounds half to even
            std::fesetround(FE_TONEAREST);
            return static_cast<int64_t>(std::nearbyint(n->GetDouble()));
        }
        default: throw RemesPathException("cannot convert " + FormatDtype(n->type) + " to integer");
    }
}

static double ToDoubleV(const JNodePtr& n) { return n->ToDouble(); }

// ── Binop implementations ──────────────────────────────────────────────────
JNodePtr Binop::Add(JNodePtr a, JNodePtr b) {
    Dtype atype = a->type, btype = b->type;
    if (BothTypesIntersect(atype, btype, DT_INT_OR_BOOL)) return JNode::Int(ToInt64(a) + ToInt64(b));
    if (BothTypesIntersect(atype, btype, DT_NUM)) return JNode::Float(ToDoubleV(a) + ToDoubleV(b));
    if (atype == DT_STR && btype == DT_STR) return JNode::Str(a->GetStr() + b->GetStr());
    throw RemesPathException("Can't add objects of types " + FormatDtype(atype) + " and " + FormatDtype(btype));
}

JNodePtr Binop::Sub(JNodePtr a, JNodePtr b) {
    Dtype atype = a->type, btype = b->type;
    if (BothTypesIntersect(atype, btype, DT_INT_OR_BOOL)) return JNode::Int(ToInt64(a) - ToInt64(b));
    if (BothTypesIntersect(atype, btype, DT_NUM)) return JNode::Float(ToDoubleV(a) - ToDoubleV(b));
    throw RemesPathException("Can't subtract objects of type " + FormatDtype(atype) + " and " + FormatDtype(btype));
}

JNodePtr Binop::Mul(JNodePtr a, JNodePtr b) {
    Dtype atype = a->type, btype = b->type;
    if ((btype & DT_INT_OR_BOOL) != 0) {
        if ((atype & DT_INT_OR_BOOL) != 0) return JNode::Int(ToInt64(a) * ToInt64(b));
        if (atype == DT_STR) return JNode::Str(ArgFunction::StrMulHelper(a->GetStr(), ToInt64(b)));
    }
    if (BothTypesIntersect(atype, btype, DT_NUM)) return JNode::Float(ToDoubleV(a) * ToDoubleV(b));
    throw RemesPathException("Can't multiply objects of type " + FormatDtype(atype) + " and " + FormatDtype(btype));
}

JNodePtr Binop::Divide(JNodePtr a, JNodePtr b) {
    if (BothTypesIntersect(a->type, b->type, DT_NUM)) return JNode::Float(ToDoubleV(a) / ToDoubleV(b));
    throw RemesPathException("Can't divide objects of types " + FormatDtype(a->type) + " and " + FormatDtype(b->type));
}

JNodePtr Binop::FloorDivide(JNodePtr a, JNodePtr b) {
    if (BothTypesIntersect(a->type, b->type, DT_NUM))
        return JNode::Int(static_cast<int64_t>(std::floor(ToDoubleV(a) / ToDoubleV(b))));
    throw RemesPathException("Can't floor divide objects of types " + FormatDtype(a->type) + " and " + FormatDtype(b->type));
}

JNodePtr Binop::Pow(JNodePtr a, JNodePtr b) {
    if (BothTypesIntersect(a->type, b->type, DT_NUM))
        return JNode::Float(std::pow(ToDoubleV(a), ToDoubleV(b)));
    throw RemesPathException("Can't exponentiate objects of type " + FormatDtype(a->type) + " and " + FormatDtype(b->type));
}

JNodePtr Binop::Mod(JNodePtr a, JNodePtr b) {
    Dtype atype = a->type, btype = b->type;
    if (BothTypesIntersect(atype, btype, DT_INT_OR_BOOL)) return JNode::Int(ToInt64(a) % ToInt64(b));
    if (BothTypesIntersect(atype, btype, DT_NUM)) return JNode::Float(std::fmod(ToDoubleV(a), ToDoubleV(b)));
    throw RemesPathException("Can't use modulo operator on objects of type " + FormatDtype(atype) + " and " + FormatDtype(btype));
}

JNodePtr Binop::BitWiseOR(JNodePtr a, JNodePtr b) {
    if (a->type == DT_BOOL && b->type == DT_BOOL) return JNode::Bool(a->GetBool() || b->GetBool());
    if (BothTypesIntersect(a->type, b->type, DT_INT_OR_BOOL)) return JNode::Int(ToInt64(a) | ToInt64(b));
    throw RemesPathException("Can't bitwise OR objects of type " + FormatDtype(a->type) + " and " + FormatDtype(b->type));
}

JNodePtr Binop::BitWiseXOR(JNodePtr a, JNodePtr b) {
    if (a->type == DT_BOOL && b->type == DT_BOOL) return JNode::Bool(a->GetBool() != b->GetBool());
    if (BothTypesIntersect(a->type, b->type, DT_INT_OR_BOOL)) return JNode::Int(ToInt64(a) ^ ToInt64(b));
    throw RemesPathException("Can't bitwise XOR objects of type " + FormatDtype(a->type) + " and " + FormatDtype(b->type));
}

JNodePtr Binop::BitWiseAND(JNodePtr a, JNodePtr b) {
    if (a->type == DT_BOOL && b->type == DT_BOOL) return JNode::Bool(a->GetBool() && b->GetBool());
    if (BothTypesIntersect(a->type, b->type, DT_INT_OR_BOOL)) return JNode::Int(ToInt64(a) & ToInt64(b));
    throw RemesPathException("Can't bitwise AND objects of type " + FormatDtype(a->type) + " and " + FormatDtype(b->type));
}

JNodePtr Binop::HasPattern(JNodePtr node, JNodePtr sub) {
    if (!node->IsStrValue())
        throw RemesPathException("For \"x =~ y\" expressions, x must be a string and y must be string or regex");
    const std::string& s = node->GetStr();
    if (auto jregex = dynamic_cast<JRegex*>(sub.get()))
        return JNode::Bool(jregex->regex->IsMatch(s));
    Regex rex(sub->GetStr());
    return JNode::Bool(rex.IsMatch(s));
}

static JNodePtr LessThanFn(JNodePtr a, JNodePtr b) { return JNode::Bool(a->CompareTo(*b) < 0); }
static JNodePtr GreaterThanFn(JNodePtr a, JNodePtr b) { return JNode::Bool(a->CompareTo(*b) > 0); }
static JNodePtr GreaterThanOrEqualFn(JNodePtr a, JNodePtr b) { return JNode::Bool(a->CompareTo(*b) >= 0); }
static JNodePtr LessThanOrEqualFn(JNodePtr a, JNodePtr b) { return JNode::Bool(a->CompareTo(*b) <= 0); }
static JNodePtr IsEqualFn(JNodePtr a, JNodePtr b) { return JNode::Bool(a->Equals(*b)); }
static JNodePtr IsNotEqualFn(JNodePtr a, JNodePtr b) { return JNode::Bool(!a->Equals(*b)); }

const std::unordered_map<std::string, BinopPtr>& Binop::BINOPS() {
    static auto* m = new std::unordered_map<std::string, BinopPtr>{
        {"&", std::make_shared<Binop>(BitWiseAND, 0, "&")},
        {"|", std::make_shared<Binop>(BitWiseOR, 0, "|")},
        {"^", std::make_shared<Binop>(BitWiseXOR, 0, "^")},
        {"=~", std::make_shared<Binop>(HasPattern, 1, "=~")},
        {"==", std::make_shared<Binop>(IsEqualFn, 1, "==")},
        {"!=", std::make_shared<Binop>(IsNotEqualFn, 1, "!=")},
        {"<", std::make_shared<Binop>(LessThanFn, 1, "<")},
        {">", std::make_shared<Binop>(GreaterThanFn, 1, ">")},
        {">=", std::make_shared<Binop>(GreaterThanOrEqualFn, 1, ">=")},
        {"<=", std::make_shared<Binop>(LessThanOrEqualFn, 1, "<=")},
        {"+", std::make_shared<Binop>(Add, 2, "+")},
        {"-", std::make_shared<Binop>(Sub, 2, "-")},
        {"//", std::make_shared<Binop>(FloorDivide, 3, "//")},
        {"%", std::make_shared<Binop>(Mod, 3, "%")},
        {"*", std::make_shared<Binop>(Mul, 3, "*")},
        {"/", std::make_shared<Binop>(Divide, 3, "/")},
        {"**", std::make_shared<Binop>(Pow, 5, "**", true)},
    };
    return *m;
}

static bool IsBooleanBinop(const std::string& name) {
    return name == "==" || name == ">" || name == "<" || name == "=~" || name == "!=" ||
           name == ">=" || name == "<=";
}
static bool IsBitwiseBinop(const std::string& name) {
    return name == "^" || name == "&" || name == "|";
}
static bool IsFloatReturningBinop(const std::string& name) { return name == "/" || name == "**"; }

int Binop::ObjectOrArrayEmpty(const JNodePtr& x) {
    if (JObject* obj = AsObj(x)) return obj->Length() == 0 ? 1 : 0;
    if (JArray* arr = AsArr(x)) return arr->Length() == 0 ? 1 : 0;
    return 2;
}

JNodePtr Binop::Call(JNodePtr left, JNodePtr right) {
    auto cjl = std::dynamic_pointer_cast<CurJson>(left);
    auto cjr = std::dynamic_pointer_cast<CurJson>(right);
    auto fn = function;
    if (cjl) {
        if (cjr)
            return std::make_shared<CurJson>(DT_UNKNOWN, [fn, cjl, cjr](JNodePtr json) {
                return fn(cjl->function(json), cjr->function(json));
            });
        return std::make_shared<CurJson>(DT_UNKNOWN, [fn, cjl, right](JNodePtr json) {
            return fn(cjl->function(json), right);
        });
    }
    if (cjr)
        return std::make_shared<CurJson>(DT_UNKNOWN, [fn, left, cjr](JNodePtr json) {
            return fn(left, cjr->function(json));
        });
    return fn(left, right);
}

JNodePtr Binop::BinopTwoJsons(JNodePtr left, JNodePtr right) {
    if (ObjectOrArrayEmpty(right) == 2) {
        if (ObjectOrArrayEmpty(left) == 2) return Call(left, right);
        return BinopJsonScalar(left, right);
    }
    if (ObjectOrArrayEmpty(left) == 2) return BinopScalarJson(left, right);
    if (JObject* robj = AsObj(right)) {
        auto dic = std::make_shared<JObject>();
        JObject* lobj = AsObj(left);
        if (!lobj || robj->Length() != lobj->Length())
            throw VectorizedArithmeticException("Tried to apply a binop to two dicts with different sets of keys");
        for (auto& rkv : robj->children) {
            JNodePtr leftVal;
            if (!lobj->TryGetValue(rkv.first, leftVal))
                throw VectorizedArithmeticException("Tried to apply a binop to two dicts with different sets of keys");
            dic->children.set(rkv.first, Call(leftVal, rkv.second));
        }
        return dic;
    }
    JArray* rarr = AsArr(right);
    JArray* larr = AsArr(left);
    if (!larr || larr->Length() != rarr->Length())
        throw VectorizedArithmeticException("Tried to perform vectorized arithmetic on two arrays of unequal length");
    auto arr = std::make_shared<JArray>();
    arr->children.reserve(rarr->Length());
    for (size_t ii = 0; ii < rarr->Length(); ii++)
        arr->children.push_back(Call(larr->children[ii], rarr->children[ii]));
    return arr;
}

JNodePtr Binop::BinopJsonScalar(JNodePtr left, JNodePtr right) {
    if (JObject* lobj = AsObj(left)) {
        auto dic = std::make_shared<JObject>();
        for (auto& lkv : lobj->children) dic->children.set(lkv.first, Call(lkv.second, right));
        return dic;
    }
    JArray* larr = AsArr(left);
    auto arr = std::make_shared<JArray>();
    for (JNodePtr& x : larr->children) arr->children.push_back(Call(x, right));
    return arr;
}

JNodePtr Binop::BinopScalarJson(JNodePtr left, JNodePtr right) {
    if (JObject* robj = AsObj(right)) {
        auto dic = std::make_shared<JObject>();
        for (auto& rkv : robj->children) dic->children.set(rkv.first, Call(left, rkv.second));
        return dic;
    }
    JArray* rarr = AsArr(right);
    auto arr = std::make_shared<JArray>();
    for (JNodePtr& x : rarr->children) arr->children.push_back(Call(left, x));
    return arr;
}

Dtype Binop::BinopOutType(Dtype ltype, Dtype rtype) {
    if (ltype == DT_UNKNOWN || rtype == DT_UNKNOWN) return DT_UNKNOWN;
    if (ltype == DT_OBJ || rtype == DT_OBJ) {
        if (ltype == DT_ARR || rtype == DT_ARR)
            throw RemesPathException("Cannot have a function of an array and an object");
        return DT_OBJ;
    }
    if (ltype == DT_ARR || rtype == DT_ARR) {
        if (ltype == DT_OBJ || rtype == DT_OBJ)
            throw RemesPathException("Cannot have a function of an array and an object");
        return DT_ARR;
    }
    if (IsBooleanBinop(name)) return DT_BOOL;
    if (!BothTypesIntersect(ltype, rtype, DT_NUM)) {
        if (name == "+" && ltype == DT_STR && rtype == DT_STR) return DT_STR;
        if (name == "*" && ltype == DT_STR && (rtype & DT_INT_OR_BOOL) != 0) return DT_STR;
        throw RemesPathException("Invalid argument types " + FormatDtype(ltype) + " and " +
                                 FormatDtype(rtype) + " for binop " + name);
    }
    if (IsBitwiseBinop(name)) {
        if (ltype == DT_BOOL && rtype == DT_BOOL) return DT_BOOL;
        if (BothTypesIntersect(ltype, rtype, DT_INT_OR_BOOL)) return DT_INT;
        throw RemesPathException("Incompatible types " + FormatDtype(ltype) + " and " +
                                 FormatDtype(rtype) + " for bitwise binop " + name);
    }
    if (name == "//") return DT_INT;
    if (IsFloatReturningBinop(name)) return DT_FLOAT;
    if (BothTypesIntersect(ltype, rtype, DT_INT)) return static_cast<Dtype>(rtype & ltype);
    return DT_FLOAT;
}

JNodePtr Binop::Resolve(JNodePtr left, JNodePtr right) {
    bool leftItbl = (left->type & DT_ITERABLE) != 0;
    bool rightItbl = (right->type & DT_ITERABLE) != 0;
    Dtype outType = BinopOutType(left->type, right->type);
    auto lcur = std::dynamic_pointer_cast<CurJson>(left);
    auto rcur = std::dynamic_pointer_cast<CurJson>(right);
    // capture a SHARED pointer: the returned CurJson closures may outlive the
    // expression parser's temporary wrapped Binops (C# relies on GC here)
    BinopPtr self = shared_from_this();
    if (lcur) {
        if (rcur) {
            if (leftItbl || rightItbl)
                return std::make_shared<CurJson>(outType, [self, lcur, rcur](JNodePtr x) {
                    return self->BinopTwoJsons(lcur->function(x), rcur->function(x));
                });
            return std::make_shared<CurJson>(outType, [self, lcur, rcur](JNodePtr x) {
                return self->Call(lcur->function(x), rcur->function(x));
            });
        }
        if (leftItbl || rightItbl)
            return std::make_shared<CurJson>(outType, [self, lcur, right](JNodePtr x) {
                return self->BinopTwoJsons(lcur->function(x), right);
            });
        return std::make_shared<CurJson>(outType, [self, lcur, right](JNodePtr x) {
            return self->Call(lcur->function(x), right);
        });
    }
    if (rcur) {
        if (leftItbl || rightItbl)
            return std::make_shared<CurJson>(outType, [self, left, rcur](JNodePtr x) {
                return self->BinopTwoJsons(left, rcur->function(x));
            });
        return std::make_shared<CurJson>(outType, [self, left, rcur](JNodePtr x) {
            return self->Call(left, rcur->function(x));
        });
    }
    if (leftItbl) {
        if (rightItbl) return BinopTwoJsons(left, right);
        return BinopJsonScalar(left, right);
    }
    if (rightItbl) return BinopScalarJson(left, right);
    return Call(left, right);
}

// ── BinopWithArgs ──────────────────────────────────────────────────────────
JNodePtr BinopWithArgs::Call() {
    if (left.bwa) left = ArgStackEntry{left.bwa->Call(), nullptr};
    if (right.bwa) right = ArgStackEntry{right.bwa->Call(), nullptr};
    return binop->Resolve(left.node, right.node);
}

ArgStackEntry BinopWithArgs::ResolveStack(std::vector<BinopWithArgsPtr>& bwaStack,
                                          std::vector<ArgStackEntry>& argStack) {
    if (argStack.empty()) {
        if (!bwaStack.empty())
            throw RemesPathException("Binop " + bwaStack.back()->binop->name + " with no left argument");
        return ArgStackEntry{};
    }
    if (bwaStack.empty()) {
        if (argStack.size() >= 2)
            throw RemesPathException("Two JNodes with no binop in between");
        return argStack[0];
    }
    BinopWithArgsPtr bwa = bwaStack.back();
    BinopPtr bop = bwa->binop;
    if (bwaStack.size() == 1) {
        if (bwa->left.empty()) {
            bwa->left = argStack.back();
            argStack.pop_back();
        }
        if (!argStack.empty()) {
            bwa->right = argStack.back();
            argStack.pop_back();
            JNodePtr result = bop->Resolve(bwa->left.node, bwa->right.node);
            bwaStack.clear();
            return ArgStackEntry{result, nullptr};
        }
        return ArgStackEntry{nullptr, bwa};
    }
    // two or more binops; they can have a contest
    ArgStackEntry lastArg = argStack.back();
    argStack.pop_back();
    if (!bwa->left.empty()) {
        bwa->right = lastArg;
        argStack.push_back(ArgStackEntry{bwa->Call(), nullptr});
        bwaStack.pop_back();
        return ResolveStack(bwaStack, argStack);
    }
    BinopWithArgsPtr leftBwa = bwaStack[bwaStack.size() - 2];
    float leftPrecedence = leftBwa->binop->precedence;
    if (bop->PrecedesLeft(leftPrecedence)) {
        bwa->left = lastArg;
        return ArgStackEntry{nullptr, bwa};
    }
    leftBwa->right = lastArg;
    bwaStack.erase(bwaStack.end() - 2);
    argStack.push_back(ArgStackEntry{leftBwa->Call(), nullptr});
    return ResolveStack(bwaStack, argStack);
}

// ── UnaryOp ────────────────────────────────────────────────────────────────
JNodePtr UnaryOp::Call(JNodePtr x) {
    if (auto cj = std::dynamic_pointer_cast<CurJson>(x)) {
        Dtype outType = (cj->type & DT_ITERABLE) != 0 ? cj->type : outputType(cj->type);
        if (outType == DT_TYPELESS)
            throw RemesPathException("Invalid argument type " + FormatDtype(cj->type) +
                                     " for unary function '" + name + "'");
        UnaryOp* self = this;
        return std::make_shared<CurJson>(outType,
                                         [self, cj](JNodePtr json) { return self->Call(cj->function(json)); });
    }
    if (JArray* arr = AsArr(x)) {
        auto out = std::make_shared<JArray>();
        for (JNodePtr& child : arr->children) out->children.push_back(function(child));
        return out;
    }
    if (JObject* obj = AsObj(x)) {
        auto out = std::make_shared<JObject>();
        for (auto& kv : obj->children) out->children.set(kv.first, function(kv.second));
        return out;
    }
    return function(x);
}

void UnaryOp::AddToStack(std::vector<UnaryOpPtr>& stack, const UnaryOpPtr& self) {
    if (!stack.empty() && stack.back()->name == name && isSelfNegating)
        stack.pop_back(); // e.g., double negatives cancel
    else
        stack.push_back(self);
}

static Dtype NumericUnaryOpsOutputType(Dtype x) {
    if (x == DT_FLOAT) return DT_FLOAT;
    if ((x & DT_INT_OR_BOOL) != 0) return DT_INT;
    return DT_TYPELESS;
}
static Dtype NotUnaryOpOutputType(Dtype x) {
    return (x & (DT_NULL | DT_STR | DT_NUM)) != 0 ? DT_BOOL : DT_TYPELESS;
}

static JNodePtr Uminus(JNodePtr x) {
    if (x->type == DT_INT) return JNode::Int(-x->GetInt());
    if (x->type == DT_FLOAT) return JNode::Float(-x->GetDouble());
    if (x->type == DT_BOOL) return JNode::Int(x->GetBool() ? -1 : 0);
    throw RemesPathException("Unary '-' can only be applied to ints, bools and floats");
}
static JNodePtr NotFn(JNodePtr x) { return JNode::Bool(!ArgFunction::ToBoolHelper(x)); }
static JNodePtr Uplus(JNodePtr x) {
    if (x->type == DT_BOOL) return JNode::Int(x->GetBool() ? 1 : 0);
    if (x->type == DT_INT) return JNode::Int(x->GetInt());
    if (x->type == DT_FLOAT) return JNode::Float(x->GetDouble());
    throw RemesPathException("Unary '-' can only be applied to ints, bools and floats");
}

const std::unordered_map<std::string, UnaryOpPtr>& UnaryOp::UNARY_OPS() {
    static auto* m = new std::unordered_map<std::string, UnaryOpPtr>{
        {"-", std::make_shared<UnaryOp>(Uminus, 4, "-", true, NumericUnaryOpsOutputType)},
        {"not", std::make_shared<UnaryOp>(NotFn, -2, "not", false, NotUnaryOpOutputType)},
        {"+", std::make_shared<UnaryOp>(Uplus, 4, "+", true, NumericUnaryOpsOutputType)},
    };
    return *m;
}

// ── ArgFunction basics ─────────────────────────────────────────────────────
void ArgFunction::CheckType(const JNodePtr& x, int64_t argNum) const {
    Dtype typeOptions = TypeOptions(argNum);
    if (!x || (x->type & typeOptions) == 0) {
        Dtype argType = x ? x->type : DT_NULL;
        throw RemesPathArgumentException(nullptr, argNum, this, argType);
    }
}

void ArgFunction::ThrowWrongArgCount(int64_t count) const {
    char correctCharAfterArg = count == maxArgs ? ')' : ',';
    std::string numArgsDescription = (minArgs == maxArgs)
        ? "(" + std::to_string(maxArgs) + " args)"
        : "(" + std::to_string(minArgs) + " - " + std::to_string(maxArgs) + " args)";
    throw RemesPathException(std::string("Expected '") + correctCharAfterArg + "' after argument " +
                             std::to_string(count) + " of function " + name + " " + numArgsDescription);
}

void ArgFunction::PadToMaxArgs(std::vector<JNodePtr>& args) const {
    if (maxArgs < INT64_MAX) {
        for (int64_t arg2 = static_cast<int64_t>(args.size()); arg2 < maxArgs; arg2++)
            args.push_back(std::make_shared<JNode>());
    }
}

static void InitializeGlobalsHook(bool containsMutation) {
    regexSearchResultsShouldBeCached = !containsMutation;
    csvDelimiterInLastQuery = '\x00';
    csvQuoteCharInLastQuery = '\x00';
}
void SetArgFunctionInitializeGlobalsHook(void (*hook)(bool)); // EngineGlobals.cpp
void ArgFunction::InitializeGlobals(bool containsMutation) { InitializeGlobalsHook(containsMutation); }
namespace {
struct HookInstaller {
    HookInstaller() { SetArgFunctionInitializeGlobalsHook(&InitializeGlobalsHook); }
} gHookInstaller;
} // namespace

bool ArgFunction::ToBoolHelper(const JNodePtr& val) {
    switch (val->type) {
        case DT_BOOL: return val->GetBool();
        case DT_INT: return val->GetInt() != 0;
        case DT_FLOAT: return val->GetDouble() != 0.0;
        case DT_STR: return !val->GetStr().empty();
        case DT_ARR: return AsArr(val)->Length() > 0;
        case DT_OBJ: return AsObj(val)->Length() > 0;
        case DT_NULL: return false;
        case DT_REGEX: return !static_cast<JRegex*>(val.get())->regex->Pattern().empty();
        default: return true;
    }
}

std::string ArgFunction::StrMulHelper(const std::string& s, int64_t num) {
    if (num <= 0) return "";
    std::string sb;
    sb.reserve(s.size() * static_cast<size_t>(num));
    for (int64_t i = 0; i < num; i++) sb += s;
    return sb;
}

JNodePtr ArgFunction::AtIndex(const JNodePtr& x, int64_t idx) {
    JArray* arr = AsArr(x);
    int64_t resolved;
    if (!arr || !WrappedIndex(static_cast<int64_t>(arr->Length()), idx, resolved))
        throw RemesPathIndexOutOfRangeException(idx, x);
    return arr->children[static_cast<size_t>(resolved)];
}

JNodePtr ArgFunction::ObjectsToJNode(const JValue& v) {
    if (std::holds_alternative<std::monostate>(v)) return std::make_shared<JNode>();
    if (std::holds_alternative<int64_t>(v)) return JNode::Int(std::get<int64_t>(v));
    if (std::holds_alternative<double>(v)) return JNode::Float(std::get<double>(v));
    if (std::holds_alternative<std::string>(v)) return JNode::Str(std::get<std::string>(v));
    return JNode::Bool(std::get<bool>(v));
}

// ── the function implementations ───────────────────────────────────────────
using Args = std::vector<JNodePtr>;

static ArgFunctionPtr Fn(const std::string& name) { return ArgFunction::FUNCTIONS().at(name); }

static JArray* ArgAsArr(const Args& args, size_t i, const char* funcName) {
    JArray* arr = AsArr(args[i]);
    if (!arr)
        throw RemesPathArgumentException(nullptr, static_cast<int64_t>(i), Fn(funcName).get(),
                                         args[i]->type);
    return arr;
}
static JObject* ArgAsObj(const Args& args, size_t i, const char* funcName) {
    JObject* obj = AsObj(args[i]);
    if (!obj)
        throw RemesPathArgumentException(nullptr, static_cast<int64_t>(i), Fn(funcName).get(),
                                         args[i]->type);
    return obj;
}

static JNodePtr And_(Args& args) {
    size_t lastIdx = args.size() - 1;
    JNodePtr curjson = args[lastIdx];
    for (size_t ii = 0; ii < lastIdx; ii++) {
        JNodePtr val = args[ii];
        JNodePtr resolvedVal = std::dynamic_pointer_cast<CurJson>(val)
            ? static_cast<CurJson*>(val.get())->function(curjson) : val;
        if (!ArgFunction::ToBoolHelper(resolvedVal)) return JNode::Bool(false);
    }
    return JNode::Bool(true);
}

static JNodePtr Or_(Args& args) {
    size_t lastIdx = args.size() - 1;
    JNodePtr curjson = args[lastIdx];
    for (size_t ii = 0; ii < lastIdx; ii++) {
        JNodePtr val = args[ii];
        JNodePtr resolvedVal = std::dynamic_pointer_cast<CurJson>(val)
            ? static_cast<CurJson*>(val.get())->function(curjson) : val;
        if (ArgFunction::ToBoolHelper(resolvedVal)) return JNode::Bool(true);
    }
    return JNode::Bool(false);
}

static JNodePtr In_(Args& args) {
    JNodePtr elt = args[0];
    JNodePtr itbl = args[1];
    if (JArray* arr = AsArr(itbl)) {
        for (JNodePtr& node : arr->children) {
            try {
                if (node->CompareTo(elt->value) == 0) return JNode::Bool(true);
            } catch (const ComparisonError&) {
                // C# CompareTo throws on type mismatch and the exception propagates;
                // mirror that by rethrowing
                throw;
            }
        }
        return JNode::Bool(false);
    }
    if (!elt->IsStrValue())
        throw RemesPathException("'in' function first argument must be string if second argument is an object.");
    JObject* obj = AsObj(itbl);
    return JNode::Bool(obj && obj->children.contains(elt->GetStr()));
}

struct StringifyArgs {
    JNodePtr elt;
    char printStyle;
    bool sortKeys;
    int indent;
    char indentChar;
};
static StringifyArgs HandleStringifyArgs(Args& args) {
    StringifyArgs out;
    out.elt = args[0];
    out.printStyle = args[1]->type == DT_NULL ? 'm' : args[1]->GetStr()[0];
    out.sortKeys = args[2]->type == DT_NULL || args[2]->GetBool();
    out.indent = 4;
    out.indentChar = ' ';
    if (args[3]->type == DT_INT) {
        out.indent = static_cast<int>(args[3]->GetInt());
    } else if (args[3]->IsStrValue() && !args[3]->GetStr().empty() && args[3]->GetStr()[0] == '\t') {
        out.indent = 1;
        out.indentChar = '\t';
    }
    return out;
}

static JNodePtr Stringify_(Args& args) {
    StringifyArgs sa = HandleStringifyArgs(args);
    switch (sa.printStyle) {
        case 'm': return JNode::Str(sa.elt->ToString(sa.sortKeys, ":", ","));
        case 'c': return JNode::Str(sa.elt->ToString(sa.sortKeys));
        case 'g': return JNode::Str(sa.elt->PrettyPrint(sa.indent, sa.sortKeys, PrettyPrintStyle::Google, JNode::I64_MAX, sa.indentChar));
        case 'w': return JNode::Str(sa.elt->PrettyPrint(sa.indent, sa.sortKeys, PrettyPrintStyle::Whitesmith, JNode::I64_MAX, sa.indentChar));
        case 'p': return JNode::Str(sa.elt->PrettyPrint(sa.indent, sa.sortKeys, PrettyPrintStyle::PPrint, JNode::I64_MAX, sa.indentChar));
        default:
            throw RemesPathArgumentException(
                "print_style (2nd arg of stringify) must be one of \"mcgwp\", got " +
                    JsonLint::CharDisplay(sa.printStyle), 1, Fn("stringify").get());
    }
}

static JNodePtr TypeOf_(Args& args) { return JNode::Str(JsonSchemaMaker::TypeName(args[0]->type)); }

static JNodePtr Len_(Args& args) {
    if (JArray* arr = AsArr(args[0])) return JNode::Int(static_cast<int64_t>(arr->Length()));
    return JNode::Int(static_cast<int64_t>(ArgAsObj(args, 0, "len")->Length()));
}

static JNodePtr Sum_(Args& args) {
    JArray* itbl = ArgAsArr(args, 0, "sum");
    double tot = 0;
    for (JNodePtr& child : itbl->children) {
        if ((child->type & DT_NUM) == 0)
            throw RemesPathException("Function Sum requires an array of all numbers");
        tot += child->ToDouble();
    }
    return JNode::Float(tot);
}

static JNodePtr Mean_(Args& args) {
    JArray* itbl = ArgAsArr(args, 0, "mean");
    double tot = 0;
    for (JNodePtr& child : itbl->children) {
        if ((child->type & DT_NUM) == 0)
            throw RemesPathException("Function Mean requires an array of all numbers");
        tot += child->ToDouble();
    }
    return JNode::Float(tot / static_cast<double>(itbl->Length()));
}

static JNodePtr Enumerate_(Args& args) {
    JArray* arr = ArgAsArr(args, 0, "enumerate");
    auto result = std::make_shared<JArray>();
    for (size_t ii = 0; ii < arr->Length(); ii++) {
        auto subarr = std::make_shared<JArray>();
        subarr->children.push_back(JNode::Int(static_cast<int64_t>(ii)));
        subarr->children.push_back(arr->children[ii]);
        result->children.push_back(subarr);
    }
    return result;
}

static JNodePtr Flatten_(Args& args) {
    JArray* itbl = ArgAsArr(args, 0, "flatten");
    int64_t iterations = args[1]->type == DT_INT ? args[1]->GetInt() : 1;
    if (args[1]->type == DT_NULL) iterations = 1;
    if (iterations == 1) {
        auto flat = std::make_shared<JArray>();
        for (JNodePtr& child : itbl->children) {
            if (JArray* childArr = AsArr(child)) {
                for (JNodePtr& grandchild : childArr->children) flat->children.push_back(grandchild);
            } else {
                flat->children.push_back(child);
            }
        }
        return flat;
    }
    JNodePtr flat = args[0];
    for (int64_t ii = 0; ii < iterations; ii++) {
        Args sub{flat, std::make_shared<JNode>()};
        flat = Flatten_(sub);
    }
    return flat;
}

static JNodePtr Index_(Args& args) {
    JArray* itblArr = ArgAsArr(args, 0, "index");
    auto& itbl = itblArr->children;
    JNodePtr elt = args[1];
    bool reverse = args[2]->type == DT_BOOL && args[2]->GetBool();
    int64_t n = static_cast<int64_t>(itbl.size());
    int64_t start = reverse ? n - 1 : 0;
    int64_t end = reverse ? -1 : n;
    int64_t increment = reverse ? -1 : 1;
    for (int64_t ii = start; ii != end; ii += increment) {
        if (itbl[static_cast<size_t>(ii)]->CompareTo(*elt) == 0) return JNode::Int(ii);
    }
    throw RemesPathException("Element " + elt->ToString() + " not found in the array");
}

static JNodePtr Max_(Args& args) {
    JArray* itbl = ArgAsArr(args, 0, "max");
    double biggest = -std::numeric_limits<double>::infinity();
    for (JNodePtr& child : itbl->children) {
        if ((child->type & DT_NUM) == 0)
            throw RemesPathException("Function Max requires an array of all numbers");
        double val = child->ToDouble();
        if (val > biggest) biggest = val;
    }
    return JNode::Float(biggest);
}

static JNodePtr Min_(Args& args) {
    JArray* itbl = ArgAsArr(args, 0, "min");
    double smallest = std::numeric_limits<double>::infinity();
    for (JNodePtr& child : itbl->children) {
        if ((child->type & DT_NUM) == 0)
            throw RemesPathException("Function Min requires an array of all numbers");
        double val = child->ToDouble();
        if (val < smallest) smallest = val;
    }
    return JNode::Float(smallest);
}

// stable sort with C# CompareTo semantics (throws on type mismatches)
static void StableSortJNodes(std::vector<JNodePtr>& v,
                             const std::function<JNodePtr(const JNodePtr&)>& key) {
    std::stable_sort(v.begin(), v.end(), [&key](const JNodePtr& a, const JNodePtr& b) {
        return key(a)->CompareTo(*key(b)) < 0;
    });
}

static JNodePtr Sorted_(Args& args) {
    JArray* arr = ArgAsArr(args, 0, "sorted");
    auto sorted = std::make_shared<JArray>();
    sorted->children = arr->children;
    bool reverse = args[1]->type == DT_BOOL && args[1]->GetBool();
    StableSortJNodes(sorted->children, [](const JNodePtr& x) { return x; });
    if (reverse) std::reverse(sorted->children.begin(), sorted->children.end());
    return sorted;
}

static JNodePtr SortBy_(Args& args) {
    JArray* arr = ArgAsArr(args, 0, "sort_by");
    bool reverse = args[2]->type == DT_BOOL && args[2]->GetBool();
    JNodePtr keyNode = args[1];
    auto sorted = std::make_shared<JArray>();
    sorted->children = arr->children;
    if (auto cj = std::dynamic_pointer_cast<CurJson>(keyNode)) {
        auto fun = cj->function;
        StableSortJNodes(sorted->children, [fun](const JNodePtr& x) { return fun(x); });
    } else if (keyNode->IsStrValue()) {
        std::string kstr = keyNode->GetStr();
        StableSortJNodes(sorted->children, [kstr](const JNodePtr& x) {
            JObject* obj = AsObj(x);
            if (!obj) throw RemesPathException("sort_by with a string key requires an array of objects");
            return obj->at(kstr);
        });
    } else {
        int64_t kint = keyNode->GetInt();
        StableSortJNodes(sorted->children,
                         [kint](const JNodePtr& x) { return ArgFunction::AtIndex(x, kint); });
    }
    if (reverse) std::reverse(sorted->children.begin(), sorted->children.end());
    return sorted;
}

static JNodePtr MaxBy_(Args& args) {
    JArray* itblArr = ArgAsArr(args, 0, "max_by");
    auto& itbl = itblArr->children;
    JNodePtr maxNode = std::make_shared<JNode>();
    if (itbl.empty()) return maxNode;
    JNodePtr keyNode = args[1];
    double maxval = -std::numeric_limits<double>::infinity();
    std::function<JNodePtr(const JNodePtr&)> key;
    if (auto cj = std::dynamic_pointer_cast<CurJson>(keyNode)) {
        auto fun = cj->function;
        key = [fun](const JNodePtr& x) { return fun(x); };
    } else if (keyNode->IsStrValue()) {
        std::string keystr = keyNode->GetStr();
        key = [keystr](const JNodePtr& x) { return AsObj(x)->at(keystr); };
    } else {
        int64_t kint = keyNode->GetInt();
        key = [kint](const JNodePtr& x) { return ArgFunction::AtIndex(x, kint); };
    }
    for (JNodePtr& x : itbl) {
        double xval = key(x)->ToDouble();
        if (xval > maxval) {
            maxNode = x;
            maxval = xval;
        }
    }
    return maxNode;
}

static JNodePtr MinBy_(Args& args) {
    JArray* itblArr = ArgAsArr(args, 0, "min_by");
    auto& itbl = itblArr->children;
    JNodePtr minNode = std::make_shared<JNode>();
    if (itbl.empty()) return minNode;
    JNodePtr keyNode = args[1];
    double minval = std::numeric_limits<double>::infinity();
    std::function<JNodePtr(const JNodePtr&)> key;
    if (auto cj = std::dynamic_pointer_cast<CurJson>(keyNode)) {
        auto fun = cj->function;
        key = [fun](const JNodePtr& x) { return fun(x); };
    } else if (keyNode->IsStrValue()) {
        std::string keystr = keyNode->GetStr();
        key = [keystr](const JNodePtr& x) { return AsObj(x)->at(keystr); };
    } else {
        int64_t kint = keyNode->GetInt();
        key = [kint](const JNodePtr& x) { return ArgFunction::AtIndex(x, kint); };
    }
    for (JNodePtr& x : itbl) {
        double xval = key(x)->ToDouble();
        if (xval < minval) {
            minNode = x;
            minval = xval;
        }
    }
    return minNode;
}

static JNodePtr RandomFrom0To1_(Args&) { return JNode::Float(RandomNextDouble()); }

static JNodePtr RandomFromSchema_(Args& args) {
    JNodePtr node = args[0];
    int64_t minArrayLength = args.size() >= 2 && args[1]->type == DT_INT && args[1]->GetInt() >= 0
        ? args[1]->GetInt() : 0;
    int64_t maxArrayLength = args.size() >= 3 && args[2]->type == DT_INT && args[2]->GetInt() >= 0
        ? args[2]->GetInt() : 10;
    bool extendedAsciiStrings = args.size() >= 4 && args[3]->type == DT_BOOL && args[3]->GetBool();
    bool usePatterns = args.size() >= 5 && args[4]->type == DT_BOOL && args[4]->GetBool();
    return RandomJsonFromSchema::RandomJson(node, minArrayLength, maxArrayLength,
                                            extendedAsciiStrings, usePatterns);
}

static JNodePtr RandomInteger_(Args& args) {
    JNodePtr startNode = args[0];
    if (startNode->type != DT_INT)
        throw RemesPathArgumentException("both args to randint must be integers", 0, Fn("randint").get());
    int64_t start = startNode->GetInt();
    JNodePtr endNode = args[1];
    if (endNode->type == DT_NULL)
        return JNode::Int(start <= 0 ? 0 : static_cast<int64_t>(RandomNext(static_cast<uint64_t>(start))));
    if (endNode->type != DT_INT)
        throw RemesPathArgumentException("both args to randint must be integers", 1, Fn("randint").get());
    int64_t end = endNode->GetInt();
    if (end <= start) return JNode::Int(start);
    return JNode::Int(start + static_cast<int64_t>(RandomNext(static_cast<uint64_t>(end - start))));
}

static JNodePtr LoopCount_(Args&) { return JNode::Int(LoopCountValue()); }

static JNodePtr Range_(Args& args) {
    bool hasStart = args[0]->type == DT_INT;
    bool hasEnd = args[1]->type == DT_INT;
    bool hasStep = args[2]->type == DT_INT;
    int64_t start = hasStart ? args[0]->GetInt() : 0;
    int64_t end = hasEnd ? args[1]->GetInt() : 0;
    int64_t step = hasStep ? args[2]->GetInt() : 0;
    auto nums = std::make_shared<JArray>();
    if (!hasStart) throw RemesPathException("First argument for range function cannot be null.");
    if (hasStep && step == 0)
        throw RemesPathException("Can't have a step size of 0 for the range function");
    if (!hasEnd && start > 0) {
        for (int64_t ii = 0; ii < start; ii++) nums->children.push_back(JNode::Int(ii));
    } else if (!hasStep && hasEnd && start < end) {
        // C# compares nullable ints, where (start < null) is false — so a
        // missing end never enters this branch
        for (int64_t ii = start; ii < end; ii++) nums->children.push_back(JNode::Int(ii));
    } else if (hasStep) {
        if (start > end && step < 0) {
            for (int64_t ii = start; ii > end; ii += step) nums->children.push_back(JNode::Int(ii));
        } else if (start < end && step > 0) {
            for (int64_t ii = start; ii < end; ii += step) nums->children.push_back(JNode::Int(ii));
        }
    }
    return nums;
}

static void RecurseUntilHelper(const JNodePtr& node, const std::function<JNodePtr(JNodePtr)>& func,
                               std::vector<JNodePtr>& results) {
    try {
        JNodePtr res = func(node);
        if (res->type == DT_BOOL && res->GetBool()) {
            results.push_back(node);
            return;
        }
    } catch (...) {
    }
    if (JArray* jar = AsArr(node)) {
        for (JNodePtr& child : jar->children) RecurseUntilHelper(child, func, results);
    } else if (JObject* obj = AsObj(node)) {
        for (auto& kv : obj->children) RecurseUntilHelper(kv.second, func, results);
    }
}

static JNodePtr RecurseUntil_(Args& args) {
    JNodePtr node = args[0];
    auto cj = std::dynamic_pointer_cast<CurJson>(args[1]);
    if (!cj)
        throw RemesPathArgumentException("second argument must be a functiont that returns a boolean",
                                         1, Fn("recurse_until").get(), args[1]->type);
    auto result = std::make_shared<JArray>();
    RecurseUntilHelper(node, cj->function, result->children);
    return result;
}

static JNodePtr Values_(Args& args) {
    JObject* obj = ArgAsObj(args, 0, "values");
    auto vals = std::make_shared<JArray>();
    for (auto& kv : obj->children) vals->children.push_back(kv.second);
    return vals;
}

static JNodePtr Keys_(Args& args) {
    JObject* obj = ArgAsObj(args, 0, "keys");
    auto ks = std::make_shared<JArray>();
    for (auto& kv : obj->children) ks->children.push_back(JNode::Str(kv.first));
    return ks;
}

static JNodePtr Items_(Args& args) {
    JObject* obj = ArgAsObj(args, 0, "items");
    auto its = std::make_shared<JArray>();
    for (auto& kv : obj->children) {
        auto subarr = std::make_shared<JArray>();
        subarr->children.push_back(JNode::Str(kv.first));
        subarr->children.push_back(kv.second);
        its->children.push_back(subarr);
    }
    return its;
}

static JNodePtr Set_(Args& args) {
    JArray* arr = ArgAsArr(args, 0, "set");
    auto set = std::make_shared<JObject>();
    for (JNodePtr& child : arr->children)
        set->children.set(child->ValueOrToString(), std::make_shared<JNode>());
    return set;
}

static JNodePtr Unique_(Args& args) {
    JArray* itbl = ArgAsArr(args, 0, "unique");
    bool isSorted = args[1]->type == DT_BOOL && args[1]->GetBool();
    // C# uses a HashSet<object>; we key by (type-class, stringified value).
    // insertion-ordered to keep output deterministic
    OrderedMap uniq;
    for (JNodePtr& val : itbl->children) {
        if ((val->type & DT_SCALAR) == 0)
            throw RemesPathArgumentException("First argument to unique must be an array of all scalars.",
                                             0, Fn("unique").get());
        std::string key;
        switch (val->type) {
            case DT_NULL: key = "n"; break;
            case DT_BOOL: key = val->GetBool() ? "b1" : "b0"; break;
            case DT_INT: case DT_FLOAT: key = "d" + JNode::DoubleToString(val->ToDouble()); break;
            default: key = "s" + val->GetStr(); break;
        }
        if (!uniq.contains(key)) uniq.set(key, val);
    }
    auto uniqList = std::make_shared<JArray>();
    for (auto& kv : uniq) uniqList->children.push_back(kv.second);
    if (isSorted) StableSortJNodes(uniqList->children, [](const JNodePtr& x) { return x; });
    return uniqList;
}

static JNodePtr Quantile_(Args& args) {
    JArray* arr = ArgAsArr(args, 0, "quantile");
    std::vector<double> sorted;
    for (JNodePtr& node : arr->children) {
        if ((node->type & DT_NUM) == 0)
            throw RemesPathException("Function Quantile requires an array of all numbers");
        sorted.push_back(node->ToDouble());
    }
    double quantile = args[1]->ToDouble();
    std::sort(sorted.begin(), sorted.end());
    if (sorted.empty()) throw RemesPathException("Cannot find quantiles of an empty array");
    if (sorted.size() == 1) return JNode::Float(sorted[0]);
    double ind = quantile * static_cast<double>(sorted.size() - 1);
    int64_t lowerInd = static_cast<int64_t>(std::floor(ind));
    double lowerVal = sorted[static_cast<size_t>(lowerInd)];
    double weightedAvg;
    if (ind != static_cast<double>(lowerInd)) {
        double upperVal = sorted[static_cast<size_t>(lowerInd + 1)];
        double fracUpper = ind - static_cast<double>(lowerInd);
        weightedAvg = upperVal * fracUpper + lowerVal * (1 - fracUpper);
    } else {
        weightedAvg = lowerVal;
    }
    return JNode::Float(weightedAvg);
}

static JNodePtr ValueCounts_(Args& args) {
    JArray* arr = ArgAsArr(args, 0, "value_counts");
    bool sortByCount = args.size() > 1 && args[1]->type == DT_BOOL && args[1]->GetBool();
    OrderedMap uniqs; // key → JArray [value, count]
    for (JNodePtr& child : arr->children) {
        if (child->type == DT_NULL)
            throw RemesPathException("Can't count occurrences of objects with null values");
        std::string key;
        switch (child->type) {
            case DT_BOOL: key = child->GetBool() ? "b1" : "b0"; break;
            case DT_INT: case DT_FLOAT: key = "d" + JNode::DoubleToString(child->ToDouble()); break;
            case DT_STR: key = "s" + child->GetStr(); break;
            default: throw RemesPathException("Can't count occurrences of non-scalars");
        }
        JNodePtr* existing = uniqs.find(key);
        if (existing) {
            JArray* pair = AsArr(*existing);
            pair->children[1] = JNode::Int(pair->children[1]->GetInt() + 1);
        } else {
            auto pair = std::make_shared<JArray>();
            pair->children.push_back(child);
            pair->children.push_back(JNode::Int(1));
            uniqs.set(key, pair);
        }
    }
    auto uniqArr = std::make_shared<JArray>();
    for (auto& kv : uniqs) uniqArr->children.push_back(kv.second);
    if (sortByCount) {
        StableSortJNodes(uniqArr->children,
                         [](const JNodePtr& x) { return AsArr(x)->children[1]; });
        std::reverse(uniqArr->children.begin(), uniqArr->children.end());
    }
    return uniqArr;
}

static JNodePtr StrCat_(Args& args) {
    std::string sb;
    for (JNodePtr& arg : args) sb += arg->ValueOrToString();
    return JNode::Str(std::move(sb));
}

static JNodePtr StringJoin_(Args& args) {
    JArray* itbl = ArgAsArr(args, 1, "s_join");
    if (itbl->Length() == 0) return JNode::Str("");
    std::string sep = args[0]->GetStr();
    std::string sb;
    if (!itbl->children[0]->IsStrValue())
        throw RemesPathException("Element 0 of the array had type " +
                                 FormatDtype(itbl->children[0]->type) +
                                 ", but the array must contain only strings.");
    sb += itbl->children[0]->GetStr();
    for (size_t ii = 1; ii < itbl->Length(); ii++) {
        sb += sep;
        if (!itbl->children[ii]->IsStrValue())
            throw RemesPathException("Element " + std::to_string(ii) + " of the array had type " +
                                     FormatDtype(itbl->children[ii]->type) +
                                     ", but the array must contain only strings.");
        sb += itbl->children[ii]->GetStr();
    }
    return JNode::Str(std::move(sb));
}

// group_by
static void AddToGroupByDict(OrderedMap& gb, bool intKey, int64_t ikey, const std::string& skey,
                             const JNodePtr& child) {
    JNodePtr val;
    if (intKey) {
        val = ArgFunction::AtIndex(child, ikey);
    } else {
        JObject* subobj = AsObj(child);
        if (!subobj) throw RemesPathException("group_by with string key requires an array of objects");
        val = subobj->at(skey);
    }
    std::string vstr = val->type == DT_STR ? val->GetStr() : val->ToString();
    JNodePtr* existing = gb.find(vstr);
    if (existing) {
        AsArr(*existing)->children.push_back(child);
    } else {
        auto group = std::make_shared<JArray>();
        group->children.push_back(child);
        gb.set(vstr, group);
    }
}

static JNodePtr GroupBySingleKey(JArray* itbl, const JNodePtr& keyNode) {
    auto gbObj = std::make_shared<JObject>();
    if (keyNode->type == DT_INT) {
        for (JNodePtr& child : itbl->children)
            AddToGroupByDict(gbObj->children, true, keyNode->GetInt(), "", child);
    } else if (keyNode->IsStrValue()) {
        for (JNodePtr& child : itbl->children)
            AddToGroupByDict(gbObj->children, false, 0, keyNode->GetStr(), child);
    } else {
        throw RemesPathArgumentException("group_by keys must be integers or strings (or an array thereof)",
                                         1, Fn("group_by").get());
    }
    return gbObj;
}

static JNodePtr GroupByRecursionHelper(JArray* itbl, JArray* keys, size_t currentIdx) {
    JNodePtr key = keys->children[currentIdx];
    if (currentIdx == keys->Length() - 1) return GroupBySingleKey(itbl, key);
    OrderedMap gb;
    if (key->type == DT_INT) {
        for (JNodePtr& child : itbl->children)
            AddToGroupByDict(gb, true, key->GetInt(), "", child);
    } else if (key->IsStrValue()) {
        for (JNodePtr& child : itbl->children)
            AddToGroupByDict(gb, false, 0, key->GetStr(), child);
    }
    auto recursiveGb = std::make_shared<JObject>();
    for (auto& kv : gb)
        recursiveGb->children.set(kv.first,
                                  GroupByRecursionHelper(AsArr(kv.second), keys, currentIdx + 1));
    return recursiveGb;
}

static JNodePtr GroupBy_(Args& args) {
    JArray* itbl = ArgAsArr(args, 0, "group_by");
    JNodePtr keyNode = args[1];
    if (JArray* arr = AsArr(keyNode)) return GroupByRecursionHelper(itbl, arr, 0);
    return GroupBySingleKey(itbl, keyNode);
}

static JNodePtr Zip_(Args& args) {
    JArray* firstArr = ArgAsArr(args, 0, "zip");
    size_t firstLen = firstArr->Length();
    for (JNodePtr& a : args) {
        JArray* arr = AsArr(a);
        if (!arr || arr->Length() != firstLen)
            throw RemesPathException("The `zip` function expects all input arrays to have equal length");
    }
    auto rst = std::make_shared<JArray>();
    for (size_t ii = 0; ii < firstLen; ii++) {
        auto sub = std::make_shared<JArray>();
        for (JNodePtr& a : args) sub->children.push_back(AsArr(a)->children[ii]);
        rst->children.push_back(sub);
    }
    return rst;
}

static JNodePtr Dict_(Args& args) {
    JArray* pairs = ArgAsArr(args, 0, "dict");
    auto rst = std::make_shared<JObject>();
    for (size_t ii = 0; ii < pairs->Length(); ii++) {
        JArray* pair = AsArr(pairs->children[ii]);
        if (!pair || pair->Length() < 2 || !pair->children[0]->IsStrValue())
            throw RemesPathException("The Dict function's argument must be an array of two-item subarrays where the first item of each subarray is a string.");
        rst->children.set(pair->children[0]->GetStr(), pair->children[1]);
    }
    return rst;
}

static JNodePtr Concat_(Args& args) {
    JNodePtr firstItbl = args[0];
    if (JArray* firstArr = AsArr(firstItbl)) {
        auto newArr = std::make_shared<JArray>();
        newArr->children = firstArr->children;
        for (size_t ii = 1; ii < args.size(); ii++) {
            JArray* arr = AsArr(args[ii]);
            if (!arr)
                throw RemesPathException("All arguments to the 'concat' function must the same type - either arrays or objects");
            for (JNodePtr& c : arr->children) newArr->children.push_back(c);
        }
        return newArr;
    }
    if (JObject* firstObj = AsObj(firstItbl)) {
        auto newObj = std::make_shared<JObject>();
        for (auto& kv : firstObj->children) newObj->children.set(kv.first, kv.second);
        for (size_t ii = 1; ii < args.size(); ii++) {
            JObject* obj = AsObj(args[ii]);
            if (!obj)
                throw RemesPathException("All arguments to the 'concat' function must the same type - either arrays or objects");
            for (auto& kv : obj->children) {
                // later args overwrite; C# Dictionary reassignment keeps original
                // insertion position, and OrderedMap::set does the same
                newObj->children.set(kv.first, kv.second);
            }
        }
        return newObj;
    }
    throw RemesPathException("All arguments to the 'concat' function must the same type - either arrays or objects");
}

static JNodePtr Append_(Args& args) {
    JArray* arr = ArgAsArr(args, 0, "append");
    auto newArr = std::make_shared<JArray>();
    newArr->children = arr->children;
    for (size_t ii = 1; ii < args.size(); ii++) newArr->children.push_back(args[ii]);
    return newArr;
}

static JNodePtr At_(Args& args) {
    JNodePtr json = args[0];
    JNodePtr indsOrKeys = args[1];
    if (JArray* arr = AsArr(json)) {
        if (JArray* indArr = AsArr(indsOrKeys)) {
            auto atList = std::make_shared<JArray>();
            for (JNodePtr& indNode : indArr->children)
                atList->children.push_back(ArgFunction::AtIndex(json, ToInt64(indNode)));
            return atList;
        }
        return ArgFunction::AtIndex(json, ToInt64(indsOrKeys));
    }
    if (JObject* obj = AsObj(json)) {
        if (JArray* indArr = AsArr(indsOrKeys)) {
            auto atList = std::make_shared<JArray>();
            for (JNodePtr& key : indArr->children) atList->children.push_back(obj->at(key->GetStr()));
            return atList;
        }
        return obj->at(indsOrKeys->GetStr());
    }
    throw RemesPathArgumentException("got type " + FormatDtype(json->type), 0, Fn("at").get());
}

static JNodePtr AddItems_(Args& args) {
    JObject* obj = ArgAsObj(args, 0, "add_items");
    auto newObj = std::make_shared<JObject>();
    for (auto& kv : obj->children) newObj->children.set(kv.first, kv.second);
    size_t ii = 1;
    while (ii + 1 < args.size() + (args.size() % 2 == 0 ? 0 : 1) && ii < args.size() - 1) {
        JNodePtr k = args[ii++];
        if (k->type != DT_STR)
            throw RemesPathException("Even-numbered args to 'add_items' function (new keys) must be strings");
        JNodePtr v = args[ii++];
        newObj->children.set(k->GetStr(), v);
    }
    return newObj;
}

static JNodePtr ToRecords_(Args& args) {
    JNodePtr arg = args[0];
    JNodePtr schema = JsonSchemaMaker::BuildSchema(arg);
    char strat = args[1]->IsStrValue() && !args[1]->GetStr().empty() ? args[1]->GetStr()[0] : 'd';
    JsonTabularizerStrategy strategy;
    switch (strat) {
        case 'd': strategy = JsonTabularizerStrategy::DEFAULT; break;
        case 'r': strategy = JsonTabularizerStrategy::FULL_RECURSIVE; break;
        case 'n': strategy = JsonTabularizerStrategy::NO_RECURSION; break;
        case 's': strategy = JsonTabularizerStrategy::STRINGIFY_ITERABLES; break;
        default:
            throw RemesPathException("ToRecords second arg must be either blank or one of\n'd': default\n'r': full recursive\n'n': no recursion\n's': stringify iterables\nSee the JSON to CSV form documentation.");
    }
    JsonTabularizer tab(strategy);
    return tab.BuildTable(arg, schema);
}

static JNodePtr Pivot_(Args& args) {
    JArray* arr = ArgAsArr(args, 0, "pivot");
    auto piv = std::make_shared<JObject>();
    if (arr->Length() == 0) return piv;
    if (AsArr(arr->children[0])) {
        int64_t by = ToInt64(args[1]);
        int64_t valCol = ToInt64(args[2]);
        for (JNodePtr& item : arr->children) {
            JArray* subarr = AsArr(item);
            JNodePtr bynode = subarr->children[static_cast<size_t>(by)];
            std::string key = bynode->ValueOrToString();
            if (!piv->children.contains(key)) piv->children.set(key, std::make_shared<JArray>());
            AsArr(piv->children.at(key))->children.push_back(subarr->children[static_cast<size_t>(valCol)]);
        }
        size_t uniqCt = piv->Length();
        for (size_t ii = 3; ii < args.size(); ii++) {
            int64_t idxCol = ToInt64(args[ii]);
            auto newSubarr = std::make_shared<JArray>();
            for (size_t jj = 0; jj < arr->Length(); jj += uniqCt)
                newSubarr->children.push_back(AsArr(arr->children[jj])->children[static_cast<size_t>(idxCol)]);
            piv->children.set(std::to_string(idxCol), newSubarr);
        }
    } else if (AsObj(arr->children[0])) {
        std::string by = args[1]->GetStr();
        std::string valCol = args[2]->GetStr();
        for (JNodePtr& item : arr->children) {
            JObject* subobj = AsObj(item);
            JNodePtr bynode = subobj->at(by);
            std::string key = bynode->ValueOrToString();
            if (!piv->children.contains(key)) piv->children.set(key, std::make_shared<JArray>());
            AsArr(piv->children.at(key))->children.push_back(subobj->at(valCol));
        }
        size_t uniqCt = piv->Length();
        for (size_t ii = 3; ii < args.size(); ii++) {
            std::string idxCol = args[ii]->GetStr();
            auto newSubarr = std::make_shared<JArray>();
            for (size_t jj = 0; jj < arr->Length(); jj += uniqCt)
                newSubarr->children.push_back(AsObj(arr->children[jj])->at(idxCol));
            piv->children.set(idxCol, newSubarr);
        }
    } else {
        throw RemesPathException("First argument to Pivot must be an array of arrays or an array of objects");
    }
    return piv;
}

static JNodePtr All_(Args& args) {
    JArray* arr = ArgAsArr(args, 0, "all");
    for (JNodePtr& item : arr->children) {
        if (item->type != DT_BOOL) throw RemesPathException("all() requires an array of all booleans");
        if (!item->GetBool()) return JNode::Bool(false);
    }
    return JNode::Bool(true);
}

static JNodePtr Any_(Args& args) {
    JArray* arr = ArgAsArr(args, 0, "any");
    for (JNodePtr& item : arr->children) {
        if (item->type != DT_BOOL) throw RemesPathException("any() requires an array of all booleans");
        if (item->GetBool()) return JNode::Bool(true);
    }
    return JNode::Bool(false);
}

// ── vectorized functions ───────────────────────────────────────────────────
static JNodePtr StrLen_(Args& args) {
    JNodePtr node = args[0];
    if (node->IsStrValue()) return JNode::Int(StrLenCodePoints(node->GetStr()));
    throw RemesPathArgumentException(nullptr, 0, Fn("s_len").get(), node->type);
}

static JNodePtr StrMul_(Args& args) {
    JNodePtr arg2 = args[1];
    if ((arg2->type & DT_INT_OR_BOOL) == 0)
        throw RemesPathArgumentException(nullptr, 1, Fn("s_mul").get(), arg2->type);
    return JNode::Str(ArgFunction::StrMulHelper(args[0]->GetStr(), ToInt64(arg2)));
}

static RegexPtr AsRegexOrCompile(const JNodePtr& sub, bool multiline = false) {
    if (auto jregex = dynamic_cast<JRegex*>(sub.get())) return jregex->regex;
    return std::make_shared<Regex>(sub->GetStr(), false, multiline);
}

static JNodePtr StrCount_(Args& args) {
    const std::string& s = args[0]->GetStr();
    RegexPtr rex = AsRegexOrCompile(args[1]);
    return JNode::Int(static_cast<int64_t>(rex->Matches(s).size()));
}

static JNodePtr StrFind_(Args& args) {
    JNodePtr node = args[0];
    JRegex* jrex = dynamic_cast<JRegex*>(args[1].get());
    if (!jrex) throw RemesPathArgumentException(nullptr, 1, Fn("s_find").get(), args[1]->type);
    auto resultList = std::make_shared<JArray>();
    for (const Regex::Match& m : jrex->regex->Matches(node->GetStr()))
        resultList->children.push_back(JNode::Str(m.value));
    return resultList;
}

// ── s_fa / s_csv machinery ─────────────────────────────────────────────────
static const char* CSV_BASE_COLUMN_REGEX =
    "([^{DELIM}\\r\\n{QUOTE}]*|{QUOTE}(?:[^{QUOTE}]|{QUOTE}{2})*{QUOTE})";
static const char* CAPTURED_INT_REGEX_STR = "([+-]?(?:0x[\\da-fA-F]+|\\d+))";
static const char* CAPTURED_NUMBER_REGEX_STR =
    "([+-]?(?:0x[\\da-fA-F]+|\\d+(?:\\.\\d*)?|\\.\\d+)(?:[eE][+-]?\\d+)?)";
static const char* NONCAPTURING_INT_REGEX_STR = "(?:[+-]?(?:0x[\\da-fA-F]+|\\d+))";
static const char* NONCAPTURING_NUMBER_REGEX_STR =
    "(?:[+-]?(?:0x[\\da-fA-F]+|\\d+(?:\\.\\d*)?|\\.\\d+)(?:[eE][+-]?\\d+)?)";

static std::string ReplaceAllSub(std::string s, const std::string& from, const std::string& to) {
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

static std::string RegexEscapeChar(char c) {
    if (c == '\t') return "\\t";
    static const std::string special = "\\^$.|?*+()[]{}";
    if (special.find(c) != std::string::npos) return std::string("\\") + c;
    return std::string(1, c);
}

std::string ArgFunction::CsvRowRegex(int64_t nColumns, char delim, const std::string& newline,
                                     char quote) {
    std::string cleanDelim = RegexEscapeChar(delim);
    std::string escapedQuote = RegexEscapeChar(quote);
    std::string escapedNewline;
    if (newline == "\r" || newline == "\\r") escapedNewline = "\\r";
    else if (newline == "\n" || newline == "\\n") escapedNewline = "\\n";
    else if (newline == "\r\n" || newline == "\\r\\n") escapedNewline = "\\r\\n";
    else
        throw RemesPathException("newline must be one of [\\r\\n, \\r, \\n] or an escaped form of one of those");
    std::string column = ReplaceAllSub(ReplaceAllSub(CSV_BASE_COLUMN_REGEX, "{QUOTE}", escapedQuote),
                                       "{DELIM}", cleanDelim);
    std::string startOfLine = "(?:\\A|(?<=" + escapedNewline + "))";
    std::string endOfLine = "(?=\\z|" + escapedNewline + ")";
    std::string sb = startOfLine;
    for (int64_t ii = 0; ii < nColumns - 1; ii++) {
        sb += column;
        sb += cleanDelim;
    }
    sb += column;
    sb += endOfLine;
    return sb;
}

static JNodePtr CsvRegexAsJNode_(Args& args) {
    JNodePtr nColumnsNode = args[0];
    if (nColumnsNode->type != DT_INT)
        throw RemesPathArgumentException("first arg to csv_regex must be integer, not " +
                                             FormatDtype(nColumnsNode->type),
                                         0, Fn("csv_regex").get());
    int64_t nColumns = nColumnsNode->GetInt();
    char delim = args[1]->IsStrValue() && !args[1]->GetStr().empty() ? args[1]->GetStr()[0] : ',';
    std::string newline = args[2]->IsStrValue() ? args[2]->GetStr() : "\r\n";
    char quote = args[3]->IsStrValue() && !args[3]->GetStr().empty() ? args[3]->GetStr()[0] : '"';
    return std::make_shared<JRegex>(std::make_shared<Regex>(
        ArgFunction::CsvRowRegex(nColumns, delim, newline, quote), false, false));
}

JNodePtr ArgFunction::TransformRegex(JNodePtr regexOrString) {
    if (auto cj = std::dynamic_pointer_cast<CurJson>(regexOrString)) {
        auto fun = cj->function;
        return std::make_shared<CurJson>(DT_REGEX, [fun](JNodePtr x) {
            return TransformRegex(fun(std::move(x)));
        });
    }
    std::string pat = dynamic_cast<JRegex*>(regexOrString.get())
        ? static_cast<JRegex*>(regexOrString.get())->regex->Pattern()
        : regexOrString->GetStr();
    // replace (INT)/(NUMBER)/(?:INT)/(?:NUMBER) not preceded by backslash
    Regex finder("(?<!\\\\)\\((?:\\?:)?(?:INT|NUMBER)\\)", false, false);
    std::string fixedPat = finder.Replace(pat, [](const Regex::Match& m) -> std::string {
        const std::string& mtch = m.value;
        if (mtch[2] == ':')
            return mtch[3] == 'I' ? NONCAPTURING_INT_REGEX_STR : NONCAPTURING_NUMBER_REGEX_STR;
        return mtch[1] == 'I' ? CAPTURED_INT_REGEX_STR : CAPTURED_NUMBER_REGEX_STR;
    });
    return std::make_shared<JRegex>(std::make_shared<Regex>(fixedPat, false, true));
}

enum class HeaderHandlingInCsv {
    SKIP_HEADER_ROWS_AS_ARRAYS,
    INCLUDE_HEADER_ROWS_AS_ARRAYS,
    MAP_HEADER_TO_ROWS,
    INCLUDE_FULL_MATCH_AS_FIRST_ITEM,
};

static std::vector<int64_t> ColumnNumbersToParseAsNumber(const Args& args,
                                                         size_t firstOptionalArgNum,
                                                         int64_t nColumns, const char* funcName) {
    std::vector<int64_t> out;
    for (size_t i = firstOptionalArgNum; i < args.size(); i++) {
        if (args[i]->type != DT_INT)
            throw RemesPathArgumentException("Arguments " + std::to_string(firstOptionalArgNum) +
                                                 " onward to " + funcName + " must be integers",
                                             static_cast<int64_t>(firstOptionalArgNum),
                                             Fn(funcName).get());
        int64_t xVal = args[i]->GetInt();
        if (nColumns >= 1 && xVal < 0) xVal += nColumns;
        out.push_back(xVal);
    }
    return out;
}

static std::string UnquoteCsvQuotedString(const std::string& str, char quoteChar) {
    if (quoteChar > 0 && !str.empty() && str[0] == quoteChar && str.size() >= 2) {
        std::string inner = str.substr(1, str.size() - 2);
        return ReplaceAllSub(inner, std::string(2, quoteChar), std::string(1, quoteChar));
    }
    return str;
}

static JNodePtr StrFindAllHelper(const std::string& text, const RegexPtr& rex, Args& args,
                                 size_t firstOptionalArgNum, const char* funcName,
                                 int64_t maxGroupNum = -1,
                                 HeaderHandlingInCsv headerHandling =
                                     HeaderHandlingInCsv::INCLUDE_HEADER_ROWS_AS_ARRAYS,
                                 char csvQuoteChar = '\x00') {
    std::vector<int64_t> columnsToParseAsNumber =
        ColumnNumbersToParseAsNumber(args, firstOptionalArgNum, maxGroupNum, funcName);
    std::sort(columnsToParseAsNumber.begin(), columnsToParseAsNumber.end());
    auto rows = std::make_shared<JArray>();
    std::vector<std::string> header;
    bool isFirstRow = true;
    auto matchEvaluator = [csvQuoteChar](const std::string& mValue, bool tryParseAsNumber,
                                         int64_t jnodePosition) -> JNodePtr {
        if (tryParseAsNumber) {
            JNodePtr parsed = JsonParser::TryParseNumber(mValue, 0,
                                                         static_cast<int64_t>(mValue.size()),
                                                         jnodePosition);
            if (csvQuoteChar > 0 && parsed->IsStrValue() && !parsed->GetStr().empty() &&
                parsed->GetStr()[0] == csvQuoteChar)
                parsed->value = UnquoteCsvQuotedString(parsed->GetStr(), csvQuoteChar);
            return parsed;
        }
        return JNode::Str(UnquoteCsvQuotedString(mValue, csvQuoteChar), jnodePosition);
    };
    int64_t minGroupNum =
        headerHandling == HeaderHandlingInCsv::INCLUDE_FULL_MATCH_AS_FIRST_ITEM ? 0 : 1;
    int64_t nColumns = minGroupNum >= maxGroupNum ? 1 : maxGroupNum + 1 - minGroupNum;
    int64_t nextMatchStart = 0;
    int64_t textLen = static_cast<int64_t>(text.size());
    while (nextMatchStart < textLen) {
        Regex::Match m = rex->FirstMatch(text, nextMatchStart);
        if (!m.success) break;
        int64_t matchStart = m.start;
        int64_t jnodePos = matchStart;
        int64_t matchEnd = matchStart + m.length;
        if (maxGroupNum < 0) {
            maxGroupNum = static_cast<int64_t>(m.groups.size()) - 1;
            nColumns = minGroupNum >= maxGroupNum ? 1 : maxGroupNum + 1 - minGroupNum;
            for (auto& c : columnsToParseAsNumber)
                if (c < 0) c += maxGroupNum;
            std::sort(columnsToParseAsNumber.begin(), columnsToParseAsNumber.end());
        }
        bool parseMatchesAsRow =
            headerHandling == HeaderHandlingInCsv::INCLUDE_HEADER_ROWS_AS_ARRAYS ||
            headerHandling == HeaderHandlingInCsv::INCLUDE_FULL_MATCH_AS_FIRST_ITEM || !isFirstRow;
        if (nColumns == 1) {
            int64_t nextColToParseAsNumber =
                columnsToParseAsNumber.empty() ? -1 : columnsToParseAsNumber[0];
            std::string mValue;
            int64_t childPos;
            if (maxGroupNum == 1 && m.groups.size() > 1) {
                const Regex::Group& grp1 = m.groups[1];
                childPos = grp1.start;
                mValue = grp1.value;
            } else {
                mValue = m.value;
                childPos = jnodePos;
            }
            if (isFirstRow && headerHandling == HeaderHandlingInCsv::MAP_HEADER_TO_ROWS)
                header = {UnquoteCsvQuotedString(mValue, csvQuoteChar)};
            if (parseMatchesAsRow) {
                JNodePtr nodeToAdd = matchEvaluator(mValue, nextColToParseAsNumber >= 0, childPos);
                if (headerHandling == HeaderHandlingInCsv::MAP_HEADER_TO_ROWS) {
                    auto thisRow = std::make_shared<JObject>(jnodePos);
                    thisRow->children.set(header[0], nodeToAdd);
                    rows->children.push_back(thisRow);
                } else {
                    rows->children.push_back(nodeToAdd);
                }
            }
        } else {
            if (isFirstRow && headerHandling == HeaderHandlingInCsv::MAP_HEADER_TO_ROWS) {
                header.clear();
                for (int64_t ii = minGroupNum; ii <= maxGroupNum; ii++)
                    header.push_back(UnquoteCsvQuotedString(m.groups[static_cast<size_t>(ii)].value,
                                                            csvQuoteChar));
            }
            if (parseMatchesAsRow) {
                // enumerate groups minGroupNum..maxGroupNum
                size_t idxInColsToParse = 0;
                int64_t nextColToParseAsNumber =
                    columnsToParseAsNumber.empty() ? -1 : columnsToParseAsNumber[0];
                std::vector<JNodePtr> rowVals;
                for (int64_t jj = minGroupNum; jj <= maxGroupNum; jj++) {
                    const Regex::Group& grp = m.groups[static_cast<size_t>(jj)];
                    int64_t jnodeIndex = grp.matched ? grp.start : matchStart;
                    JNodePtr item;
                    if (!grp.matched) {
                        item = JNode::Str("", jnodeIndex);
                    } else if (nextColToParseAsNumber == jj - minGroupNum) {
                        item = matchEvaluator(grp.value, true, jnodeIndex);
                        idxInColsToParse++;
                        nextColToParseAsNumber = columnsToParseAsNumber.size() > idxInColsToParse
                            ? columnsToParseAsNumber[idxInColsToParse] : -1;
                    } else {
                        item = matchEvaluator(grp.value, false, jnodeIndex);
                    }
                    rowVals.push_back(item);
                }
                if (headerHandling == HeaderHandlingInCsv::MAP_HEADER_TO_ROWS) {
                    auto rowObj = std::make_shared<JObject>(jnodePos);
                    for (size_t ii = 0; ii < rowVals.size(); ii++)
                        rowObj->children.set(header[ii], rowVals[ii]);
                    rows->children.push_back(rowObj);
                } else {
                    auto row = std::make_shared<JArray>(jnodePos);
                    row->children = std::move(rowVals);
                    rows->children.push_back(row);
                }
            }
        }
        nextMatchStart = matchEnd > matchStart ? matchEnd : matchStart + 1;
        isFirstRow = false;
    }
    return rows;
}

static JNodePtr CsvRead_(Args& args) {
    std::string text = args[0]->GetStr();
    JNodePtr arg2 = args[1];
    if (arg2->type != DT_INT)
        throw RemesPathArgumentException(nullptr, 1, Fn("s_csv").get(), arg2->type);
    int64_t nColumns = arg2->GetInt();
    char delim = args.size() > 2 && args[2]->IsStrValue() ? args[2]->GetStr()[0] : ',';
    csvDelimiterInLastQuery = delim;
    std::string newline = args.size() > 3 && args[3]->IsStrValue() ? args[3]->GetStr() : "\r\n";
    char quote = args.size() > 4 && args[4]->IsStrValue() ? args[4]->GetStr()[0] : '"';
    csvQuoteCharInLastQuery = quote;
    std::string headerHandlingAbbrev = args.size() > 5 && args[5]->IsStrValue() ? args[5]->GetStr() : "n";
    HeaderHandlingInCsv headerHandling;
    if (headerHandlingAbbrev == "n") headerHandling = HeaderHandlingInCsv::SKIP_HEADER_ROWS_AS_ARRAYS;
    else if (headerHandlingAbbrev == "h") headerHandling = HeaderHandlingInCsv::INCLUDE_HEADER_ROWS_AS_ARRAYS;
    else if (headerHandlingAbbrev == "d") headerHandling = HeaderHandlingInCsv::MAP_HEADER_TO_ROWS;
    else
        throw RemesPathArgumentException(
            "header_handling (6th argument, default 'n') must be 'n' (no header, rows as arrays), 'h' (include header, rows as arrays), or 'd' (rows as objects with header as keys)",
            5, Fn("s_csv").get());
    std::string rexPat = ArgFunction::CsvRowRegex(nColumns, delim, newline, quote);
    auto rex = std::make_shared<Regex>(rexPat, false, false);
    return StrFindAllHelper(text, rex, args, 6, "s_csv", nColumns, headerHandling, quote);
}

static JNodePtr ToCsv_(Args& args) {
    JArray* arr = ArgAsArr(args, 0, "to_csv");
    if (arr->Length() == 0) return JNode::Str("");
    char delim = args[1]->IsStrValue() ? args[1]->GetStr()[0] : ',';
    std::string newline = args[2]->IsStrValue() ? args[2]->GetStr() : "\r\n";
    char quote = args[3]->IsStrValue() ? args[3]->GetStr()[0] : '"';
    std::string sb;
    JNodePtr firstRow = arr->children[0];
    int64_t nColumns = 0;
    if (JObject* obj = AsObj(firstRow)) {
        std::vector<std::string> keys = obj->children.keys();
        nColumns = static_cast<int64_t>(keys.size());
        for (size_t ii = 0; ii < keys.size(); ii++) {
            JsonTabularizer::ApplyQuotesIfNeeded(sb, keys[ii], delim, quote);
            if (ii < keys.size() - 1) sb += delim;
        }
        sb += newline;
        for (JNodePtr& child : arr->children) {
            JObject* o = AsObj(child);
            if (!o) throw RemesPathException("to_csv requires an array of all objects or all arrays");
            size_t ii = 0;
            for (auto& kv : o->children) {
                JsonTabularizer::CsvStringToSb(sb, kv.second, delim, quote, false);
                ii++;
                if (ii < o->Length()) sb += delim;
            }
            nColumns = static_cast<int64_t>(ii);
            sb += newline;
        }
    } else {
        for (JNodePtr& child : arr->children) {
            if (JArray* a = AsArr(child)) {
                nColumns = static_cast<int64_t>(a->Length());
                for (size_t ii = 0; ii < a->Length(); ii++) {
                    JsonTabularizer::CsvStringToSb(sb, a->children[ii], delim, quote, false);
                    if (ii < a->Length() - 1) sb += delim;
                }
                sb += newline;
            } else {
                JsonTabularizer::CsvStringToSb(sb, child, delim, quote, false);
                sb += newline;
                nColumns = 1;
            }
        }
    }
    // 1-column CSVs drop the trailing newline (it would read as an empty final row)
    if (nColumns == 1) sb.resize(sb.size() - newline.size());
    return JNode::Str(std::move(sb));
}

static JNodePtr StrFindAll_(Args& args) {
    std::string text = args[0]->GetStr();
    JRegex* jrex = dynamic_cast<JRegex*>(args[1].get());
    if (!jrex) throw RemesPathArgumentException(nullptr, 1, Fn("s_fa").get(), args[1]->type);
    bool includeFullMatchAsFirstItem = args.size() > 2 && args[2]->type == DT_BOOL && args[2]->GetBool();
    HeaderHandlingInCsv headerHandling = includeFullMatchAsFirstItem
        ? HeaderHandlingInCsv::INCLUDE_FULL_MATCH_AS_FIRST_ITEM
        : HeaderHandlingInCsv::INCLUDE_HEADER_ROWS_AS_ARRAYS;
    return StrFindAllHelper(text, jrex->regex, args, 3, "s_fa", -1, headerHandling);
}

// splits like .NET Regex.Split: pieces between matches, with capture-group
// values of each match inserted between the pieces
static JNodePtr RegexSplitToArray(const std::string& s, const Regex& rex) {
    auto outNodes = std::make_shared<JArray>();
    std::vector<Regex::Match> ms = rex.Matches(s);
    int64_t last = 0;
    for (const Regex::Match& m : ms) {
        outNodes->children.push_back(
            JNode::Str(s.substr(static_cast<size_t>(last), static_cast<size_t>(m.start - last))));
        for (size_t g = 1; g < m.groups.size(); g++) {
            if (m.groups[g].matched) outNodes->children.push_back(JNode::Str(m.groups[g].value));
        }
        last = m.start + m.length;
    }
    outNodes->children.push_back(JNode::Str(s.substr(static_cast<size_t>(last))));
    return outNodes;
}

static JNodePtr StrSplit_(Args& args) {
    const std::string& s = args[0]->GetStr();
    JNodePtr sepNode = args[1];
    if (sepNode->type == DT_NULL) {
        Regex ws("\\s+", false, false);
        return RegexSplitToArray(s, ws);
    }
    if (sepNode->IsStrValue()) {
        Regex rex(sepNode->GetStr(), false, false);
        return RegexSplitToArray(s, rex);
    }
    JRegex* jrex = dynamic_cast<JRegex*>(sepNode.get());
    if (!jrex) throw RemesPathArgumentException(nullptr, 1, Fn("s_split").get(), sepNode->type);
    return RegexSplitToArray(s, *jrex->regex);
}

static JNodePtr StrSplitLines_(Args& args) {
    const std::string& s = args[0]->GetStr();
    Regex rex("\\r\\n?|\\n", false, false);
    return RegexSplitToArray(s, rex);
}

static JNodePtr StrLower_(Args& args) { return JNode::Str(StrToLower(args[0]->GetStr())); }
static JNodePtr StrUpper_(Args& args) { return JNode::Str(StrToUpper(args[0]->GetStr())); }
static JNodePtr StrStrip_(Args& args) { return JNode::Str(StrTrim(args[0]->GetStr())); }

static JNodePtr StrSlice_(Args& args) {
    const std::string& s = args[0]->GetStr();
    JNodePtr slicerOrInt = args[1];
    if (auto jslicer = dynamic_cast<JSlicer*>(slicerOrInt.get()))
        return JNode::Str(StrSlice(s, jslicer->slicer));
    int64_t index = ToInt64(slicerOrInt);
    std::string result;
    if (!StrWrappedIndex(s, index, result))
        throw RemesPathIndexOutOfRangeException(index, args[0]);
    return JNode::Str(std::move(result));
}

static JNodePtr StrSub_(Args& args) {
    const std::string& val = args[0]->GetStr();
    JNodePtr toReplace = args[1];
    JNodePtr repl = args[2];
    if (JRegex* jRegex = dynamic_cast<JRegex*>(toReplace.get())) {
        if (auto cj = std::dynamic_pointer_cast<CurJson>(repl)) {
            auto fun = cj->function;
            int64_t previousLoopCountValue = LoopCountValue();
            LoopCountValue() = 0;
            std::string result = jRegex->regex->Replace(val, [&fun](const Regex::Match& m) {
                auto matchArr = std::make_shared<JArray>();
                for (const Regex::Group& g : m.groups)
                    matchArr->children.push_back(JNode::Str(g.matched ? g.value : ""));
                int64_t loopCountBeforeCall = LoopCountValue();
                LoopCountValue() = loopCountBeforeCall + 1;
                JNodePtr out = fun(matchArr);
                if (!out->IsStrValue())
                    throw RemesPathException("s_sub callback must return a string");
                return out->GetStr();
            });
            LoopCountValue() = previousLoopCountValue;
            return JNode::Str(std::move(result));
        }
        return JNode::Str(jRegex->regex->Replace(val, repl->GetStr()));
    }
    // string-to-string replacement
    return JNode::Str(ReplaceAllSub(val, toReplace->GetStr(), repl->GetStr()));
}

static std::string LeftPadHelper(const std::string& s, const std::string& padWith, int64_t padToLen) {
    int64_t lengthOfPad = padToLen - StrLenCodePoints(s);
    std::string sb;
    int64_t padLen = StrLenCodePoints(padWith);
    int64_t padWithCount = padLen == 0 ? 0 : lengthOfPad / padLen;
    if (padLen != 0 && lengthOfPad % padLen != 0) padWithCount++;
    for (int64_t ii = 0; ii < padWithCount; ii++) sb += padWith;
    sb += s;
    return sb;
}

static std::string RightPadHelper(const std::string& s, const std::string& padWith, int64_t padToLen) {
    int64_t lengthOfPad = padToLen - StrLenCodePoints(s);
    std::string sb = s;
    int64_t padLen = StrLenCodePoints(padWith);
    int64_t padWithCount = padLen == 0 ? 0 : lengthOfPad / padLen;
    if (padLen != 0 && lengthOfPad % padLen != 0) padWithCount++;
    for (int64_t ii = 0; ii < padWithCount; ii++) sb += padWith;
    return sb;
}

static JNodePtr StrLeftPad_(Args& args) {
    return JNode::Str(LeftPadHelper(args[0]->GetStr(), args[1]->GetStr(), args[2]->GetInt()));
}
static JNodePtr StrRightPad_(Args& args) {
    return JNode::Str(RightPadHelper(args[0]->GetStr(), args[1]->GetStr(), args[2]->GetInt()));
}

static JNodePtr ZFill_(Args& args) {
    std::string s = args[0]->ValueOrToString();
    int64_t padToLen = args[1]->GetInt();
    int64_t len = StrLenCodePoints(s);
    if (len >= padToLen) return JNode::Str(std::move(s));
    return JNode::Str(std::string(static_cast<size_t>(padToLen - len), '0') + s);
}

static JNodePtr StrFormat_(Args& args) {
    StringifyArgs sa = HandleStringifyArgs(args);
    if (!sa.elt->IsStrValue())
        throw RemesPathArgumentException(nullptr, 0, Fn("s_format").get(), sa.elt->type);
    const std::string& s = sa.elt->GetStr();
    bool rememberComments = args[4]->type != DT_NULL && args[4]->type == DT_BOOL && args[4]->GetBool();
    JsonParser parser(LoggerLevel::JSON5, false, false, rememberComments);
    JNodePtr result = parser.Parse(s);
    if (parser.fatal()) return JNode::Str(s);
    if (rememberComments) {
        switch (sa.printStyle) {
            case 'm': case 'c':
                return JNode::Str(result->ToStringWithComments(parser.comments, sa.sortKeys));
            case 'g': case 'w':
                return JNode::Str(result->PrettyPrintWithComments(parser.comments, sa.indent,
                                                                  sa.sortKeys, sa.indentChar,
                                                                  PrettyPrintStyle::Google));
            case 'p':
                return JNode::Str(result->PrettyPrintWithComments(parser.comments, sa.indent,
                                                                  sa.sortKeys, sa.indentChar,
                                                                  PrettyPrintStyle::PPrint));
            default:
                throw RemesPathArgumentException(
                    "print_style (2nd arg of s_format) must be one of \"mcgwp\", got " +
                        JsonLint::CharDisplay(sa.printStyle), 1, Fn("s_format").get());
        }
    }
    switch (sa.printStyle) {
        case 'm': return JNode::Str(result->ToString(sa.sortKeys, ":", ","));
        case 'c': return JNode::Str(result->ToString(sa.sortKeys));
        case 'g': return JNode::Str(result->PrettyPrint(sa.indent, sa.sortKeys, PrettyPrintStyle::Google, JNode::I64_MAX, sa.indentChar));
        case 'w': return JNode::Str(result->PrettyPrint(sa.indent, sa.sortKeys, PrettyPrintStyle::Whitesmith, JNode::I64_MAX, sa.indentChar));
        case 'p': return JNode::Str(result->PrettyPrint(sa.indent, sa.sortKeys, PrettyPrintStyle::PPrint, JNode::I64_MAX, sa.indentChar));
        default:
            throw RemesPathArgumentException(
                "print_style (2nd arg of s_format) must be one of \"mcgwp\", got " +
                    JsonLint::CharDisplay(sa.printStyle), 1, Fn("s_format").get());
    }
}

static JNodePtr IsStr_(Args& args) { return JNode::Bool(args[0]->type == DT_STR); }
static JNodePtr IsNum_(Args& args) { return JNode::Bool((args[0]->type & DT_NUM) != 0); }
static JNodePtr IsExpr_(Args& args) { return JNode::Bool((args[0]->type & DT_ARR_OR_OBJ) != 0); }
static JNodePtr IsNull_(Args& args) { return JNode::Bool(args[0]->type == DT_NULL); }

static JNodePtr IfElse_(Args& args) {
    JNodePtr val = ArgFunction::ToBoolHelper(args[0]) ? args[1] : args[2];
    if (auto cj = std::dynamic_pointer_cast<CurJson>(val))
        return cj->function(args[3]); // secret 4th arg: CurJson(Identity) for conditional execution
    return val;
}

static JNodePtr Log_(Args& args) {
    JNodePtr arg1 = args[0];
    if ((arg1->type & DT_FLOAT_OR_INT) == 0)
        throw RemesPathArgumentException(nullptr, 0, Fn("log").get(), arg1->type);
    double num = arg1->ToDouble();
    if (args[1]->type == DT_NULL) return JNode::Float(std::log(num));
    return JNode::Float(std::log(num) / std::log(args[1]->ToDouble()));
}

static JNodePtr Log2_(Args& args) {
    JNodePtr arg1 = args[0];
    if ((arg1->type & DT_FLOAT_OR_INT) == 0)
        throw RemesPathArgumentException(nullptr, 0, Fn("log").get(), arg1->type);
    return JNode::Float(std::log2(arg1->ToDouble()));
}

static JNodePtr Abs_(Args& args) {
    JNodePtr val = args[0];
    if ((val->type & DT_INT_OR_BOOL) != 0) return JNode::Int(std::llabs(ToInt64(val)));
    if (val->type == DT_FLOAT) return JNode::Float(std::fabs(val->GetDouble()));
    throw RemesPathArgumentException(nullptr, 0, Fn("abs").get(), val->type);
}

static JNodePtr IsNa_(Args& args) { return JNode::Bool(std::isnan(args[0]->ToDouble())); }
static JNodePtr ToBool_(Args& args) { return JNode::Bool(ArgFunction::ToBoolHelper(args[0])); }
static JNodePtr ToStr_(Args& args) { return JNode::Str(args[0]->ValueOrToString()); }

static JNodePtr ToFloat_(Args& args) {
    JNodePtr val = args[0];
    if (val->IsStrValue()) {
        errno = 0;
        char* endp = nullptr;
        double d = strtod(val->GetStr().c_str(), &endp);
        if (endp == val->GetStr().c_str() || *endp != '\0')
            throw RemesPathException("could not parse \"" + val->GetStr() + "\" as a number");
        return JNode::Float(d);
    }
    return JNode::Float(val->ToDouble());
}

static JNodePtr ToInt_(Args& args) {
    JNodePtr val = args[0];
    if (val->IsStrValue()) {
        errno = 0;
        char* endp = nullptr;
        long long l = strtoll(val->GetStr().c_str(), &endp, 10);
        if (endp == val->GetStr().c_str() || *endp != '\0' || errno == ERANGE)
            throw RemesPathException("could not parse \"" + val->GetStr() + "\" as an integer");
        return JNode::Int(l);
    }
    return JNode::Int(ToInt64(val));
}

static JNodePtr ToNum_(Args& args) {
    JNodePtr val = args[0];
    if (val->IsStrValue()) {
        const std::string& s = val->GetStr();
        if (s.empty()) throw RemesPathException("num() got an empty string");
        char startChar = s[0];
        bool hasLeadingSign = startChar == '-' || startChar == '+';
        size_t xpos = hasLeadingSign ? 2 : 1;
        if (s.size() > xpos + 1 && s[xpos] == 'x') {
            errno = 0;
            char* endp = nullptr;
            long long l = strtoll(s.c_str() + xpos + 1, &endp, 16);
            if (*endp != '\0') throw RemesPathException("could not parse \"" + s + "\" as a number");
            double d = static_cast<double>(l);
            return JNode::Float(startChar == '-' ? -d : d);
        }
        errno = 0;
        char* endp = nullptr;
        double d = strtod(s.c_str(), &endp);
        if (endp == s.c_str() || *endp != '\0')
            throw RemesPathException("could not parse \"" + s + "\" as a number");
        return JNode::Float(d);
    }
    return JNode::Float(val->ToDouble());
}

static JNodePtr Round_(Args& args) {
    JNodePtr val = args[0];
    JNodePtr sigfigs = args[1];
    if (val->type == DT_INT) return JNode::Int(val->GetInt());
    if (val->type == DT_FLOAT) {
        double d = val->GetDouble();
        std::fesetround(FE_TONEAREST); // C# Math.Round = banker's rounding
        if (sigfigs->type == DT_NULL) return JNode::Int(static_cast<int64_t>(std::nearbyint(d)));
        double scale = std::pow(10.0, static_cast<double>(ToInt64(sigfigs)));
        return JNode::Float(std::nearbyint(d * scale) / scale);
    }
    throw RemesPathArgumentException(nullptr, 0, Fn("round").get(), val->type);
}

static JNodePtr Parse_(Args& args) {
    const std::string& stringified = args[0]->GetStr();
    auto output = std::make_shared<JObject>();
    JsonParser parser(LoggerLevel::JSON5, false, false);
    JNodePtr result = parser.Parse(stringified);
    if (parser.fatal()) {
        const JsonLint* fatalError = parser.fatalError();
        output->children.set(
            "error", JNode::Str(fatalError->message() + " at position " +
                                std::to_string(fatalError->pos) + " (char " +
                                JsonLint::CharDisplay(fatalError->curChar) + ")"));
    } else {
        output->children.set("result", result);
    }
    return output;
}

// ── the FUNCTIONS registry ─────────────────────────────────────────────────
std::unordered_map<std::string, ArgFunctionPtr>& ArgFunction::FUNCTIONS() {
    static auto* m = new std::unordered_map<std::string, ArgFunctionPtr>();
    if (!m->empty()) return *m;
    constexpr int64_t MANY = INT64_MAX;
    auto add = [&](const char* name, std::function<JNodePtr(Args&)> fn, Dtype type,
                   int64_t minA, int64_t maxA, bool vec, std::vector<Dtype> types,
                   bool deterministic = true, std::shared_ptr<ArgsTransform> at = nullptr,
                   bool condExec = false) {
        (*m)[name] = std::make_shared<ArgFunction>(std::move(fn), name, type, minA, maxA, vec,
                                                   std::move(types), deterministic, std::move(at),
                                                   condExec);
    };
    auto D = [](int t) { return static_cast<Dtype>(t); };
    // non-vectorized
    add("add_items", AddItems_, DT_OBJ, 3, MANY, false, {DT_OBJ, DT_STR, DT_ANYTHING, DT_ANYTHING});
    add("all", All_, DT_BOOL, 1, 1, false, {DT_ARR});
    add("and", And_, DT_BOOL, 2, MANY, false,
        {D(DT_ANYTHING | DT_FUNCTION), D(DT_ANYTHING | DT_FUNCTION), D(DT_ANYTHING | DT_FUNCTION)},
        true, nullptr, true);
    add("any", Any_, DT_BOOL, 1, 1, false, {DT_ARR});
    add("append", Append_, DT_ARR, 2, MANY, false, {DT_ARR, DT_ANYTHING, DT_ANYTHING});
    add("at", At_, DT_ANYTHING, 2, 2, false, {DT_ARR_OR_OBJ, D(DT_ARR | DT_INT | DT_STR)});
    add("avg", Mean_, DT_FLOAT, 1, 1, false, {DT_ARR});
    add("concat", Concat_, DT_ARR_OR_OBJ, 2, MANY, false, {DT_ITERABLE, DT_ITERABLE, DT_ITERABLE});
    add("csv_regex", CsvRegexAsJNode_, DT_REGEX, 1, 4, false, {DT_INT, DT_STR, DT_STR, DT_STR});
    add("dict", Dict_, DT_OBJ, 1, 1, false, {DT_ARR});
    add("enumerate", Enumerate_, DT_ARR, 1, 1, false, {DT_ARR});
    add("flatten", Flatten_, DT_ARR, 1, 2, false, {DT_ARR, DT_INT});
    add("group_by", GroupBy_, DT_OBJ, 2, 2, false, {DT_ARR, D(DT_STR | DT_INT | DT_ARR)});
    add("in", In_, DT_BOOL, 2, 2, false, {DT_ANYTHING, DT_ITERABLE});
    add("index", Index_, DT_INT, 2, 3, false, {DT_ITERABLE, DT_SCALAR, DT_BOOL});
    add("items", Items_, DT_ARR, 1, 1, false, {DT_OBJ});
    add("iterable", IsExpr_, DT_BOOL, 1, 1, false, {DT_ANYTHING});
    add("keys", Keys_, DT_ARR, 1, 1, false, {DT_OBJ});
    add("len", Len_, DT_INT, 1, 1, false, {DT_ITERABLE});
    add("loop", LoopCount_, DT_INT, 0, 0, false, {}, false);
    add("max", Max_, DT_FLOAT, 1, 1, false, {DT_ARR});
    add("max_by", MaxBy_, DT_ANYTHING, 2, 2, false, {DT_ARR, D(DT_STR | DT_INT | DT_FUNCTION)});
    add("mean", Mean_, DT_FLOAT, 1, 1, false, {DT_ARR});
    add("min", Min_, DT_FLOAT, 1, 1, false, {DT_ARR});
    add("min_by", MinBy_, DT_ANYTHING, 2, 2, false, {DT_ARR, D(DT_STR | DT_INT | DT_FUNCTION)});
    add("or", Or_, DT_BOOL, 2, MANY, false,
        {D(DT_ANYTHING | DT_FUNCTION), D(DT_ANYTHING | DT_FUNCTION), D(DT_ANYTHING | DT_FUNCTION)},
        true, nullptr, true);
    add("pivot", Pivot_, DT_OBJ, 3, MANY, false,
        {DT_ARR, D(DT_STR | DT_INT), D(DT_STR | DT_INT), D(DT_STR | DT_INT)});
    add("quantile", Quantile_, DT_FLOAT, 2, 2, false, {DT_ARR, DT_FLOAT});
    add("rand", RandomFrom0To1_, DT_FLOAT, 0, 0, false, {}, false);
    add("rand_schema", RandomFromSchema_, DT_ANYTHING, 1, 5, false,
        {DT_OBJ, DT_INT, DT_INT, DT_BOOL, DT_BOOL}, false);
    add("randint", RandomInteger_, DT_INT, 1, 2, false, {DT_INT, DT_INT}, false);
    add("range", Range_, DT_ARR, 1, 3, false, {DT_INT, DT_INT, DT_INT});
    add("recurse_until", RecurseUntil_, DT_ARR, 2, 2, false,
        {DT_ARR_OR_OBJ, D(DT_FUNCTION | DT_ANYTHING)}, true, nullptr, true);
    add("s_cat", StrCat_, DT_STR, 1, MANY, false, {DT_ANYTHING, DT_ANYTHING});
    add("s_join", StringJoin_, DT_STR, 2, 2, false, {DT_STR, DT_ARR});
    add("set", Set_, DT_OBJ, 1, 1, false, {DT_ARR});
    add("sort_by", SortBy_, DT_ARR, 2, 3, false, {DT_ARR, D(DT_STR | DT_INT | DT_FUNCTION), DT_BOOL});
    add("sorted", Sorted_, DT_ARR, 1, 2, false, {DT_ARR, DT_BOOL});
    add("stringify", Stringify_, DT_STR, 1, 4, false,
        {DT_ANYTHING, D(DT_NULL | DT_STR), D(DT_BOOL | DT_STR), D(DT_INT | DT_STR | DT_NULL)});
    add("sum", Sum_, DT_FLOAT, 1, 1, false, {DT_ARR});
    add("to_csv", ToCsv_, DT_STR, 1, 4, false, {DT_ARR, DT_STR, DT_STR, DT_STR});
    add("to_records", ToRecords_, DT_ARR, 1, 2, false, {DT_ITERABLE, DT_STR});
    add("type", TypeOf_, DT_STR, 1, 1, false, {DT_ANYTHING});
    add("unique", Unique_, DT_ARR, 1, 2, false, {DT_ARR, DT_BOOL});
    add("value_counts", ValueCounts_, DT_ARR, 1, 2, false, {DT_ARR, DT_BOOL});
    add("values", Values_, DT_ARR, 1, 1, false, {DT_OBJ});
    add("zip", Zip_, DT_ARR, 2, MANY, false, {DT_ARR, DT_ARR, DT_ARR});
    // vectorized
    add("abs", Abs_, DT_FLOAT_OR_INT, 1, 1, true, {D(DT_FLOAT_OR_INT | DT_ITERABLE)});
    add("bool", ToBool_, DT_BOOL, 1, 1, true, {DT_ANYTHING});
    add("float", ToFloat_, DT_FLOAT, 1, 1, true, {DT_ANYTHING});
    add("ifelse", IfElse_, DT_UNKNOWN, 3, 3, true,
        {DT_ANYTHING, D(DT_ANYTHING | DT_FUNCTION), D(DT_ANYTHING | DT_FUNCTION)}, true, nullptr, true);
    add("int", ToInt_, DT_INT, 1, 1, true, {DT_ANYTHING});
    add("is_expr", IsExpr_, DT_BOOL, 1, 1, true, {DT_ANYTHING});
    add("is_num", IsNum_, DT_BOOL, 1, 1, true, {DT_ANYTHING});
    add("is_str", IsStr_, DT_STR, 1, 1, true, {DT_ANYTHING});
    add("isna", IsNa_, DT_BOOL, 1, 1, true, {DT_ANYTHING});
    add("isnull", IsNull_, DT_BOOL, 1, 1, true, {DT_ANYTHING});
    add("log", Log_, DT_FLOAT, 1, 2, true, {D(DT_FLOAT_OR_INT | DT_ITERABLE), DT_FLOAT_OR_INT});
    add("log2", Log2_, DT_FLOAT, 1, 1, true, {D(DT_FLOAT_OR_INT | DT_ITERABLE)});
    add("num", ToNum_, DT_FLOAT, 1, 1, true, {DT_ANYTHING});
    add("parse", Parse_, DT_OBJ, 1, 1, true, {D(DT_STR | DT_ITERABLE)});
    add("round", Round_, DT_FLOAT_OR_INT, 1, 2, true, {D(DT_FLOAT_OR_INT | DT_ITERABLE), DT_INT});
    add("s_count", StrCount_, DT_INT, 2, 2, true, {D(DT_STR | DT_ITERABLE), DT_STR_OR_REGEX});
    add("s_csv", CsvRead_, DT_ARR, 2, MANY, true,
        {D(DT_STR | DT_ITERABLE), DT_INT, D(DT_STR | DT_NULL), D(DT_STR | DT_NULL),
         D(DT_STR | DT_NULL), D(DT_STR | DT_NULL), DT_INT},
        true,
        std::make_shared<ArgsTransform>(std::vector<ArgsTransform::Entry>{
            {2, DT_NULL, [](JNodePtr) { return JNode::Str(","); }},
            {3, DT_NULL, [](JNodePtr) { return JNode::Str("\r\n"); }},
            {4, DT_NULL, [](JNodePtr) { return JNode::Str("\""); }},
            {5, DT_NULL, [](JNodePtr) { return JNode::Str("n"); }}}));
    add("s_fa", StrFindAll_, DT_ARR, 2, MANY, true,
        {D(DT_STR | DT_ITERABLE), DT_STR_OR_REGEX, D(DT_BOOL | DT_NULL), DT_INT}, true,
        std::make_shared<ArgsTransform>(std::vector<ArgsTransform::Entry>{
            {1, DT_STR_OR_REGEX, [](JNodePtr x) { return ArgFunction::TransformRegex(std::move(x)); }},
            {2, DT_NULL, [](JNodePtr) { return JNode::Bool(false); }}}));
    add("s_find", StrFind_, DT_ARR, 2, 2, true, {D(DT_STR | DT_ITERABLE), DT_REGEX});
    add("s_format", StrFormat_, DT_STR, 1, 5, true,
        {DT_ANYTHING, D(DT_NULL | DT_STR), D(DT_BOOL | DT_STR), D(DT_INT | DT_STR | DT_NULL),
         D(DT_BOOL | DT_NULL)});
    add("s_len", StrLen_, DT_INT, 1, 1, true, {D(DT_STR | DT_ITERABLE)});
    add("s_lines", StrSplitLines_, DT_ARR, 1, 1, true, {D(DT_STR | DT_ITERABLE)});
    add("s_lower", StrLower_, DT_STR, 1, 1, true, {D(DT_STR | DT_ITERABLE)});
    add("s_lpad", StrLeftPad_, DT_STR, 3, 3, true,
        {D(DT_STR | DT_ITERABLE), D(DT_STR | DT_ITERABLE), D(DT_INT | DT_ITERABLE)});
    add("s_mul", StrMul_, DT_STR, 2, 2, true, {D(DT_STR | DT_ITERABLE), DT_INT});
    add("s_rpad", StrRightPad_, DT_STR, 3, 3, true,
        {D(DT_STR | DT_ITERABLE), D(DT_STR | DT_ITERABLE), D(DT_INT | DT_ITERABLE)});
    add("s_slice", StrSlice_, DT_STR, 2, 2, true, {D(DT_STR | DT_ITERABLE), DT_INT_OR_SLICE});
    add("s_split", StrSplit_, DT_ARR, 1, 2, true, {D(DT_STR | DT_ITERABLE), DT_STR_OR_REGEX});
    add("s_strip", StrStrip_, DT_STR, 1, 1, true, {D(DT_STR | DT_ITERABLE)});
    add("s_sub", StrSub_, DT_STR, 3, 3, true,
        {D(DT_STR | DT_ITERABLE), DT_STR_OR_REGEX, D(DT_STR | DT_FUNCTION)}, true,
        std::make_shared<ArgsTransform>(std::vector<ArgsTransform::Entry>{
            {1, DT_REGEX, [](JNodePtr x) { return ArgFunction::TransformRegex(std::move(x)); }}}));
    add("s_upper", StrUpper_, DT_STR, 1, 1, true, {D(DT_STR | DT_ITERABLE)});
    add("str", ToStr_, DT_STR, 1, 1, true, {DT_ANYTHING});
    add("zfill", ZFill_, DT_STR, 2, 2, true, {DT_ANYTHING, D(DT_INT | DT_ITERABLE)});
    return *m;
}

void ArgsTransform::Transform(std::vector<JNodePtr>& args) const {
    for (const Entry& e : transformers_) {
        if (static_cast<int>(args.size()) > e.index) {
            JNodePtr arg = args[static_cast<size_t>(e.index)];
            if ((arg->type & e.typesToTransform) != 0)
                args[static_cast<size_t>(e.index)] = e.transformer(arg);
        }
    }
}

} // namespace jsontools
