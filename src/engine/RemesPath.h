// RemesPath.h — the RemesPath query language for the JsonTools macOS port.
// Faithful port of RemesPathLexer.cs, RemesPathFunctions.cs and RemesPath.cs.
//
// Port notes:
// * C#'s List<object> token stream becomes a tagged Token struct.
// * The C# lazy IEnumerable indexers are materialized into vectors; the C#
//   code mutates IndexerFunc state (hasOneOption/isDict) mid-iteration from
//   inside generator bodies, which the port models with a per-run result
//   struct (IndexerRun) instead of mutating shared state.
// * Binop/UnaryOp hold std::function so the expression parser can wrap them
//   (unary-op fusion) exactly like the C# does.
// * string indexing/slicing (s_slice, WrappedIndex) operates on Unicode CODE
//   POINTS here; the C# operates on UTF-16 code units. Identical outside the
//   astral plane.

#pragma once

#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "JNode.h"
#include "JsonParser.h"

namespace jsontools {

// ── exceptions ─────────────────────────────────────────────────────────────
struct RemesPathException : JsonToolsError {
    explicit RemesPathException(const std::string& description) : JsonToolsError(description) {}
};
struct VectorizedArithmeticException : RemesPathException {
    using RemesPathException::RemesPathException;
};
struct RemesPathIndexOutOfRangeException : RemesPathException {
    RemesPathIndexOutOfRangeException(int64_t index, const JNodePtr& node)
        : RemesPathException("Index " + std::to_string(index) + " out of range for " +
                             FormatDtype(node->type) + " " + node->ToString()) {}
};
struct RemesLexerException : JsonToolsError {
    int64_t lexpos;
    std::string query;
    std::string msg;
    RemesLexerException(int64_t lexpos_, std::string query_, std::string msg_)
        : JsonToolsError(MarkLocationOfSyntaxError(query_, lexpos_, msg_)),
          lexpos(lexpos_), query(std::move(query_)), msg(std::move(msg_)) {}
    explicit RemesLexerException(const std::string& msg_)
        : JsonToolsError("Error: " + msg_), lexpos(0), msg(msg_) {}
    static std::string MarkLocationOfSyntaxError(const std::string& query, int64_t lexpos,
                                                 const std::string& msg);
};

class ArgFunction;
struct RemesPathArgumentException : RemesPathException {
    RemesPathArgumentException(const char* description, int64_t argNum, const ArgFunction* func,
                               Dtype gotType = DT_NULL);
    RemesPathArgumentException(const std::string& description, int64_t argNum,
                               const ArgFunction* func, Dtype gotType = DT_NULL);
};

// ── slicing helpers (ArrayExtensions.cs) ───────────────────────────────────
// A slicer is 1-3 (hasValue, value) pairs, like C#'s int?[].
using Slicer = std::vector<std::pair<bool, int64_t>>;
int64_t ClampWithinLen(int64_t len, int64_t num, bool isStartIdx);
// appends the indices selected by (start, stop, stride) over a length-len list
void ResolveSlice(int64_t len, const Slicer& slicer, std::vector<int64_t>& indicesOut);
// python-style wrapped index; returns false if out of bounds
bool WrappedIndex(int64_t len, int64_t idx, int64_t& resolved);
// code-point based string slicing / indexing
std::string StrSlice(const std::string& s, const Slicer& slicer);
bool StrWrappedIndex(const std::string& s, int64_t idx, std::string& atIdx);
// code-point count of a UTF-8 string (C# s.Length counts UTF-16 units)
int64_t StrLenCodePoints(const std::string& s);

// mac-backed Unicode string helpers (implemented in Regex.mm)
std::string StrToLower(const std::string& s);
std::string StrToUpper(const std::string& s);
std::string StrTrim(const std::string& s);

// ── LruCache (Utils/LruCache.cs) ───────────────────────────────────────────
template <typename K, typename V>
class LruCache {
public:
    explicit LruCache(size_t capacity = 64) : capacity_(capacity) {}
    bool TryGetValue(const K& key, V& val) {
        auto it = map_.find(key);
        if (it == map_.end()) return false;
        useOrder_.splice(useOrder_.end(), useOrder_, it->second.second);
        val = it->second.first;
        return true;
    }
    // only sets if not already present (C# SetDefault)
    void SetDefault(const K& key, const V& val) {
        if (map_.count(key)) return;
        Set(key, val);
    }
    void Set(const K& key, const V& val) {
        auto it = map_.find(key);
        if (it != map_.end()) {
            it->second.first = val;
            useOrder_.splice(useOrder_.end(), useOrder_, it->second.second);
            return;
        }
        if (map_.size() >= capacity_) {
            map_.erase(useOrder_.front());
            useOrder_.pop_front();
        }
        useOrder_.push_back(key);
        auto last = useOrder_.end();
        --last;
        map_.emplace(key, std::make_pair(val, last));
    }
    void Clear() { map_.clear(); useOrder_.clear(); }
    bool ContainsKey(const K& key) const { return map_.count(key) > 0; }
    bool IsFull() const { return map_.size() >= capacity_; }
    size_t Count() const { return map_.size(); }
    size_t UseOrderCount() const { return useOrder_.size(); }
    size_t Capacity() const { return capacity_; }
    // the least recently used key; only valid when Count() > 0
    const K& OldestKey() const { return useOrder_.front(); }
    bool UseOrderContains(const K& key) const {
        for (const K& k : useOrder_)
            if (k == key) return true;
        return false;
    }

private:
    size_t capacity_;
    std::list<K> useOrder_;
    std::unordered_map<K, std::pair<V, typename std::list<K>::iterator>> map_;
};

// ── Binop ──────────────────────────────────────────────────────────────────
class Binop;
using BinopPtr = std::shared_ptr<Binop>;

class Binop : public std::enable_shared_from_this<Binop> {
public:
    std::function<JNodePtr(JNodePtr, JNodePtr)> function;
    float precedence;
    std::string name;
    bool isRightAssociative;

    Binop(std::function<JNodePtr(JNodePtr, JNodePtr)> fn, float prec, std::string name_,
          bool rightAssoc = false)
        : function(std::move(fn)), precedence(prec), name(std::move(name_)),
          isRightAssociative(rightAssoc) {}

    bool PrecedesLeft(float leftPrecedence) const {
        if (leftPrecedence > precedence) return false;
        if (leftPrecedence < precedence) return true;
        return isRightAssociative;
    }
    JNodePtr Call(JNodePtr left, JNodePtr right);
    JNodePtr BinopTwoJsons(JNodePtr left, JNodePtr right);
    JNodePtr BinopJsonScalar(JNodePtr left, JNodePtr right);
    JNodePtr BinopScalarJson(JNodePtr left, JNodePtr right);
    Dtype BinopOutType(Dtype ltype, Dtype rtype);
    JNodePtr Resolve(JNodePtr left, JNodePtr right);

    // returns 2 if x is not obj/arr, 1 if empty obj/arr, 0 otherwise
    static int ObjectOrArrayEmpty(const JNodePtr& x);
    static const std::unordered_map<std::string, BinopPtr>& BINOPS();

    // binop implementations
    static JNodePtr Add(JNodePtr a, JNodePtr b);
    static JNodePtr Sub(JNodePtr a, JNodePtr b);
    static JNodePtr Mul(JNodePtr a, JNodePtr b);
    static JNodePtr Divide(JNodePtr a, JNodePtr b);
    static JNodePtr FloorDivide(JNodePtr a, JNodePtr b);
    static JNodePtr Pow(JNodePtr a, JNodePtr b);
    static JNodePtr Mod(JNodePtr a, JNodePtr b);
    static JNodePtr BitWiseOR(JNodePtr a, JNodePtr b);
    static JNodePtr BitWiseXOR(JNodePtr a, JNodePtr b);
    static JNodePtr BitWiseAND(JNodePtr a, JNodePtr b);
    static JNodePtr HasPattern(JNodePtr node, JNodePtr sub);
};

class BinopWithArgs;
using BinopWithArgsPtr = std::shared_ptr<BinopWithArgs>;

// argStack entries and BinopWithArgs operands: JNodePtr or BinopWithArgsPtr
struct ArgStackEntry {
    JNodePtr node;          // non-null if JNode
    BinopWithArgsPtr bwa;   // non-null if BinopWithArgs
    bool empty() const { return !node && !bwa; }
};

class BinopWithArgs {
public:
    BinopPtr binop;
    ArgStackEntry left;
    ArgStackEntry right;

    BinopWithArgs(BinopPtr b, ArgStackEntry l, ArgStackEntry r)
        : binop(std::move(b)), left(std::move(l)), right(std::move(r)) {}
    JNodePtr Call();
    // returns JNodePtr result OR the pending BinopWithArgs (null,null if argStack empty)
    static ArgStackEntry ResolveStack(std::vector<BinopWithArgsPtr>& bwaStack,
                                      std::vector<ArgStackEntry>& argStack);
};

// ── UnaryOp ────────────────────────────────────────────────────────────────
class UnaryOp;
using UnaryOpPtr = std::shared_ptr<UnaryOp>;

class UnaryOp {
public:
    std::function<JNodePtr(JNodePtr)> function;
    float precedence;
    std::string name;
    bool isSelfNegating;
    std::function<Dtype(Dtype)> outputType;

    UnaryOp(std::function<JNodePtr(JNodePtr)> fn, float prec, std::string name_,
            bool selfNegating, std::function<Dtype(Dtype)> outType)
        : function(std::move(fn)), precedence(prec), name(std::move(name_)),
          isSelfNegating(selfNegating), outputType(std::move(outType)) {}

    JNodePtr Call(JNodePtr x);
    void AddToStack(std::vector<UnaryOpPtr>& stack, const UnaryOpPtr& self);
    static const std::unordered_map<std::string, UnaryOpPtr>& UNARY_OPS();
};

// ── ArgFunction ────────────────────────────────────────────────────────────
class ArgsTransform {
public:
    struct Entry {
        int index;
        Dtype typesToTransform;
        std::function<JNodePtr(JNodePtr)> transformer;
    };
    explicit ArgsTransform(std::vector<Entry> entries) : transformers_(std::move(entries)) {}
    void Transform(std::vector<JNodePtr>& args) const;

private:
    std::vector<Entry> transformers_;
};

class ArgFunction {
public:
    std::function<JNodePtr(std::vector<JNodePtr>&)> Call;
    std::vector<Dtype> inputTypes;
    std::string name;
    Dtype type;
    int64_t maxArgs;
    int64_t minArgs;
    bool isVectorized;
    bool isDeterministic;
    std::shared_ptr<ArgsTransform> argsTransform;
    bool conditionalExecution;

    ArgFunction(std::function<JNodePtr(std::vector<JNodePtr>&)> fn, std::string name_,
                Dtype type_, int64_t minArgs_, int64_t maxArgs_, bool isVectorized_,
                std::vector<Dtype> inputTypes_, bool isDeterministic_ = true,
                std::shared_ptr<ArgsTransform> argsTransform_ = nullptr,
                bool conditionalExecution_ = false)
        : Call(std::move(fn)), inputTypes(std::move(inputTypes_)), name(std::move(name_)),
          type(type_), maxArgs(maxArgs_), minArgs(minArgs_), isVectorized(isVectorized_),
          isDeterministic(isDeterministic_), argsTransform(std::move(argsTransform_)),
          conditionalExecution(conditionalExecution_) {}

    Dtype TypeOptions(int64_t argNum) const {
        Dtype t = inputTypes.empty()
            ? DT_ANYTHING
            : (argNum >= static_cast<int64_t>(inputTypes.size()) ? inputTypes.back()
                                                                 : inputTypes[static_cast<size_t>(argNum)]);
        return static_cast<Dtype>(t | DT_UNKNOWN);
    }
    Dtype OutputType(const JNodePtr& x) const {
        return isVectorized && (x->type & DT_ITERABLE) != 0 ? x->type : type;
    }
    void CheckType(const JNodePtr& x, int64_t argNum) const;
    [[noreturn]] void ThrowWrongArgCount(int64_t count) const;
    void PadToMaxArgs(std::vector<JNodePtr>& args) const;

    static const OrderedMap& FunctionsPlaceholder(); // unused; see FUNCTIONS()
    static std::unordered_map<std::string, std::shared_ptr<ArgFunction>>& FUNCTIONS();
    static void InitializeGlobals(bool containsMutation);

    // shared helpers also used elsewhere
    static bool ToBoolHelper(const JNodePtr& val);
    static std::string StrMulHelper(const std::string& s, int64_t num);
    static JNodePtr AtIndex(const JNodePtr& x, int64_t idx);
    static JNodePtr ObjectsToJNode(const JValue& v); // scalars only in this port
    static JNodePtr TransformRegex(JNodePtr regexOrString);
    static std::string CsvRowRegex(int64_t nColumns, char delim = ',',
                                   const std::string& newline = "\r\n", char quote = '"');
};
using ArgFunctionPtr = std::shared_ptr<ArgFunction>;

class ArgFunctionWithArgs {
public:
    ArgFunctionPtr function;
    std::vector<JNodePtr> args;
    ArgFunctionWithArgs(ArgFunctionPtr fn, std::vector<JNodePtr> args_)
        : function(std::move(fn)), args(std::move(args_)) {}
    JNodePtr Call() { return function->Call(args); }
};

// ── lexer ──────────────────────────────────────────────────────────────────
struct UnquotedString {
    std::string value;
};

struct Token {
    enum Kind { JNODE, DELIM, BINOP, UNOP, UQS } kind = JNODE;
    JNodePtr node;
    char delim = 0;
    BinopPtr binop;
    UnaryOpPtr unop;
    std::string uqs;
    int64_t sourceIndex = 0; // byte position in the query (for error messages)

    static Token Node(JNodePtr n, int64_t idx = 0) { Token t; t.kind = JNODE; t.node = std::move(n); t.sourceIndex = idx; return t; }
    static Token Delim(char c, int64_t idx = 0) { Token t; t.kind = DELIM; t.delim = c; t.sourceIndex = idx; return t; }
    static Token Bop(BinopPtr b, int64_t idx = 0) { Token t; t.kind = BINOP; t.binop = std::move(b); t.sourceIndex = idx; return t; }
    static Token Unop(UnaryOpPtr u, int64_t idx = 0) { Token t; t.kind = UNOP; t.unop = std::move(u); t.sourceIndex = idx; return t; }
    static Token Uqs(std::string s, int64_t idx = 0) { Token t; t.kind = UQS; t.uqs = std::move(s); t.sourceIndex = idx; return t; }
    bool IsDelim(char c) const { return kind == DELIM && delim == c; }
};

class RemesPathLexer {
public:
    static constexpr int MAX_RECURSION_DEPTH = JsonParser::MAX_RECURSION_DEPTH;
    JsonParser jsonParser;

    RemesPathLexer() : jsonParser(LoggerLevel::JSON5, false, false, false) {}

    std::vector<Token> Tokenize(const std::string& q);
    Token ParseUnquotedString(const std::string& unquoted, int64_t sourceIndex);
    static void BraceMatchCheck(const std::string& q, const std::vector<Token>& toks, char open,
                                char close);
    static std::string StringToBacktickString(const std::string& s);

    static const std::unordered_map<std::string, VariableAssignmentType>& VAR_ASSIGN_KEYWORDS_TO_TYPES();
    static const char* LOOP_END_KEYWORD; // "end"

private:
    void TokenizeFString(std::vector<Token>& tokens, const std::string& fstring,
                         const std::string& query, int64_t startIndex);
};

// ── indexers ───────────────────────────────────────────────────────────────
struct IndexerResult {
    bool isKV = false;
    std::string key;
    JNodePtr node;
};

struct IndexerRun {
    std::vector<IndexerResult> items;
    bool hasOneOption = false;
    bool isDict = false;
};

struct IndexerFunc {
    // fills run.items; may override run.hasOneOption/run.isDict (boolean index)
    std::function<void(JNodePtr, IndexerRun&)> run;
    bool hasOneOption = false;
    bool isProjection = false;
    bool isDict = false;
    bool isRecursive = false;
};

// VarnameList/SlicerList child: string key | regex | int | slicer
struct IndexerListValue {
    enum Kind { STRING, REGEX, INT, SLICER } kind = STRING;
    std::string str;
    RegexPtr regex;
    int64_t intval = 0;
    Slicer slicer;
};

// ── parser ─────────────────────────────────────────────────────────────────
class RemesParser {
public:
    RemesPathLexer lexer;
    LruCache<std::string, JNodePtr> cache;

    explicit RemesParser(size_t cacheCapacity = 64) : cache(cacheCapacity) {}

    // compile a query into a JNode (CurJson/JQueryContext when input-dependent)
    JNodePtr Compile(const std::string& query);
    // compile + evaluate on obj
    JNodePtr Search(const std::string& query, JNodePtr obj);

    static std::string PrettifyException(const std::exception& ex);

    static const char* EXPR_FUNC_ENDERS; // "]:},);"

private:
    struct ObjPosNode { JNodePtr node; int64_t pos; };

    enum class IndexerStart {
        NOT_AN_INDEXER, SQUAREBRACE, CURLYBRACE, DOT, DOUBLEDOT, DOUBLEDOT_SQUAREBRACE,
        FORWARD_ARROW, BANG_SQUAREBRACE, BANG_DOT,
    };
    struct ParsedIndexer {
        enum Kind { VARNAME_LIST, SLICER_LIST, STAR, BOOLEAN, PROJECTION_IDX } kind;
        std::vector<IndexerListValue> children; // varname/slicer lists
        JNodePtr booleanValue;
        std::function<void(JNodePtr, IndexerRun&)> projFunc;
        Dtype projReturnType = DT_UNKNOWN;
    };
    struct ParsedIndexerPos { ParsedIndexer idx; int64_t pos; };

    JNodePtr ParseQuery(std::vector<Token>& toks);
    std::unordered_map<std::string, bool> DetermineWhichVariablesAreFunctionsOfInput(
        const std::vector<Token>& toks);
    std::pair<int64_t, bool> CheckStatementForInputReferences(
        const std::vector<Token>& toks, int64_t pos, int64_t equalsPosition,
        int64_t endOfStatement, std::unordered_map<std::string, bool>& isVarnameFunctionOfInput,
        bool containsMutation);
    JNodePtr ParseStatement(std::vector<Token>& toks, int64_t start, int64_t end,
                            const std::shared_ptr<JQueryContext>& context,
                            std::unordered_map<std::string, bool>& isVarnameFunctionOfInput);
    static bool StatementIsVarAssign(const std::vector<Token>& toks, int64_t start, int64_t end,
                                     std::string& varname, VariableAssignmentType& assignmentType);
    static bool StatementIsLoopEnd(const std::vector<Token>& toks, int64_t start, int64_t end);

    ObjPosNode ParseExprFunc(std::vector<Token>& toks, int64_t pos, int64_t end,
                             const std::shared_ptr<JQueryContext>& context);
    ObjPosNode ParseExpr(std::vector<Token>& toks, int64_t pos, int64_t end,
                         const std::shared_ptr<JQueryContext>& context);
    ObjPosNode ParseArgFunction(std::vector<Token>& toks, int64_t pos, const ArgFunctionPtr& fun,
                                int64_t end, const std::shared_ptr<JQueryContext>& context);
    JNodePtr SpreadArrayToArgFunctionArgs(const std::vector<JNodePtr>& args, JNodePtr curArg,
                                          const ArgFunctionPtr& fun);
    ParsedIndexerPos ParseIndexer(std::vector<Token>& toks, int64_t pos, int64_t end,
                                  IndexerStart indStart,
                                  const std::shared_ptr<JQueryContext>& context);
    ParsedIndexerPos ParseProjection(std::vector<Token>& toks, int64_t pos, int64_t end,
                                     const std::shared_ptr<JQueryContext>& context);
    ParsedIndexerPos ParseMap(std::vector<Token>& toks, int64_t pos, int64_t end,
                              const std::shared_ptr<JQueryContext>& context);
    struct SlicerPos { Slicer slicer; int64_t pos; };
    SlicerPos ParseSlicer(std::vector<Token>& toks, int64_t pos, bool hasFirstNum,
                          int64_t firstNum, int64_t end,
                          const std::shared_ptr<JQueryContext>& context);
    JNodePtr ParseNonFunctionUnquotedStr(int64_t tokenIndex, const std::string& uqs,
                                         const std::shared_ptr<JQueryContext>& context);
    JNodePtr ApplyArgFunction(ArgFunctionWithArgs& func);

    static std::pair<IndexerStart, int64_t> DetermineIndexerStart(const std::vector<Token>& toks,
                                                                  int64_t pos, int64_t end);
    static int64_t FindTokenIndex(const std::vector<Token>& toks, char delim, int64_t start,
                                  int64_t end);
};

// ── RemesPath globals set by s_csv (used by the UI for CSV detection) ──────
extern char csvDelimiterInLastQuery;
extern char csvQuoteCharInLastQuery;
extern bool regexSearchResultsShouldBeCached; // declared in JNode.cpp / EngineGlobals.cpp
int64_t& LoopCountValue(); // the loop() counter used by s_sub callbacks

// random source shared with RandomJsonFromSchema
uint64_t RandomNext(uint64_t exclusiveUpperBound);
double RandomNextDouble();

} // namespace jsontools
