// RemesPath.cpp — the RemesPath query compiler. Port of RemesPath.cs's
// RemesParser (indexers, projections, the expression parser).

#include "RemesPath.h"

#include <algorithm>

namespace jsontools {

const char* RemesParser::EXPR_FUNC_ENDERS = "]:},);";

static bool IsExprFuncEnder(char c) {
    return c == ']' || c == ':' || c == '}' || c == ',' || c == ')' || c == ';';
}

JNodePtr RemesParser::Compile(const std::string& query) {
    JNodePtr oldResult;
    if (cache.TryGetValue(query, oldResult)) return oldResult;
    std::vector<Token> toks = lexer.Tokenize(query);
    JNodePtr result = ParseQuery(toks);
    cache.SetDefault(query, result);
    return result;
}

JNodePtr RemesParser::Search(const std::string& query, JNodePtr obj) {
    JNodePtr compiledQuery = Compile(query);
    if (compiledQuery->CanOperate()) return compiledQuery->Operate(obj);
    return compiledQuery;
}

int64_t RemesParser::FindTokenIndex(const std::vector<Token>& toks, char delim, int64_t start,
                                    int64_t end) {
    for (int64_t i = start; i < end; i++)
        if (toks[static_cast<size_t>(i)].IsDelim(delim)) return i;
    return -1;
}

// ── query / statements ─────────────────────────────────────────────────────
JNodePtr RemesParser::ParseQuery(std::vector<Token>& toks) {
    int64_t pos = 0;
    int64_t end = static_cast<int64_t>(toks.size());
    auto context = std::make_shared<JQueryContext>();
    std::unordered_map<std::string, bool> isVarnameFunctionOfInput =
        DetermineWhichVariablesAreFunctionsOfInput(toks);
    while (pos < end) {
        int64_t nextSemicolon = FindTokenIndex(toks, ';', pos, end);
        int64_t endOfStatement = nextSemicolon < 0 ? end : nextSemicolon;
        JNodePtr statement = ParseStatement(toks, pos, endOfStatement, context, isVarnameFunctionOfInput);
        context->AddStatement(statement);
        pos = endOfStatement + 1;
    }
    return context->GetQuery();
}

std::unordered_map<std::string, bool> RemesParser::DetermineWhichVariablesAreFunctionsOfInput(
    const std::vector<Token>& toks) {
    std::unordered_map<std::string, bool> isVarnameFunctionOfInput;
    int64_t pos = 0;
    int64_t end = static_cast<int64_t>(toks.size());
    bool containsMutation = false;
    while (pos < end) {
        int64_t nextSemicolon = FindTokenIndex(toks, ';', pos, end);
        int64_t endOfStatement = nextSemicolon < 0 ? end : nextSemicolon;
        int64_t nextEqualsSign = FindTokenIndex(toks, '=', pos, end);
        if (nextEqualsSign >= 0 && nextEqualsSign < endOfStatement) {
            auto res = CheckStatementForInputReferences(toks, pos, nextEqualsSign, endOfStatement,
                                                        isVarnameFunctionOfInput, containsMutation);
            pos = res.first;
            containsMutation = res.second;
        } else {
            pos = endOfStatement + 1;
        }
    }
    ArgFunction::InitializeGlobals(containsMutation);
    if (containsMutation) {
        for (auto& kv : isVarnameFunctionOfInput) kv.second = true;
    }
    return isVarnameFunctionOfInput;
}

std::pair<int64_t, bool> RemesParser::CheckStatementForInputReferences(
    const std::vector<Token>& toks, int64_t pos, int64_t equalsPosition, int64_t endOfStatement,
    std::unordered_map<std::string, bool>& isVarnameFunctionOfInput, bool containsMutation) {
    std::string varname;
    VariableAssignmentType assignmentType;
    if (StatementIsVarAssign(toks, pos, endOfStatement, varname, assignmentType)) {
        if (containsMutation || assignmentType == VariableAssignmentType::LOOP) {
            // loop variables are re-cached per iteration, effectively functions of input
            isVarnameFunctionOfInput[varname] = true;
            return {endOfStatement + 1, containsMutation};
        }
    } else {
        // mutator expression: assume all variables are functions of input
        return {endOfStatement + 1, true};
    }
    for (pos = equalsPosition + 1; pos < endOfStatement; pos++) {
        const Token& tok = toks[static_cast<size_t>(pos)];
        bool isCurJson = tok.kind == Token::JNODE && dynamic_cast<CurJson*>(tok.node.get());
        bool isInputVar = tok.kind == Token::UQS &&
            isVarnameFunctionOfInput.count(tok.uqs) && isVarnameFunctionOfInput[tok.uqs];
        if (isCurJson || isInputVar) {
            isVarnameFunctionOfInput[varname] = true;
            return {endOfStatement + 1, false};
        }
    }
    auto it = isVarnameFunctionOfInput.find(varname);
    if (!(it != isVarnameFunctionOfInput.end() && it->second))
        isVarnameFunctionOfInput[varname] = false;
    return {endOfStatement + 1, false};
}

JNodePtr RemesParser::ParseStatement(std::vector<Token>& toks, int64_t start, int64_t end,
                                     const std::shared_ptr<JQueryContext>& context,
                                     std::unordered_map<std::string, bool>& isVarnameFunctionOfInput) {
    int64_t indexOfAssignment = FindTokenIndex(toks, '=', start, end);
    if (indexOfAssignment == start)
        throw RemesPathException("Assignment operator '=' with no left-hand side");
    if (indexOfAssignment == end - 1)
        throw RemesPathException("Assignment operator '=' with no right-hand side");
    if (indexOfAssignment >= 0) {
        if (FindTokenIndex(toks, '=', indexOfAssignment + 1, end) >= 0)
            throw RemesPathException("Only one '=' assignment operator allowed in a statement");
        std::string varname;
        VariableAssignmentType assignmentType;
        if (StatementIsVarAssign(toks, start, end, varname, assignmentType)) {
            bool isFunctionOfInput = isVarnameFunctionOfInput[varname];
            JNodePtr value = ParseExprFunc(toks, start + 3, end, context).node;
            return std::make_shared<VarAssign>(varname, value,
                                               static_cast<int>(context->indexInStatements),
                                               assignmentType, isFunctionOfInput);
        }
        JNodePtr selector = ParseExprFunc(toks, start, indexOfAssignment, context).node;
        JNodePtr mutator = ParseExprFunc(toks, indexOfAssignment + 1, end, context).node;
        return std::make_shared<JMutator>(selector, mutator);
    }
    if (StatementIsLoopEnd(toks, start, end)) return std::make_shared<LoopEnd>();
    return ParseExprFunc(toks, start, end, context).node;
}

bool RemesParser::StatementIsVarAssign(const std::vector<Token>& toks, int64_t start, int64_t end,
                                       std::string& varname,
                                       VariableAssignmentType& assignmentType) {
    varname.clear();
    assignmentType = VariableAssignmentType::INVALID;
    if (end <= static_cast<int64_t>(toks.size()) && start <= end - 4 &&
        toks[static_cast<size_t>(start)].kind == Token::UQS) {
        auto it = RemesPathLexer::VAR_ASSIGN_KEYWORDS_TO_TYPES().find(toks[static_cast<size_t>(start)].uqs);
        if (it != RemesPathLexer::VAR_ASSIGN_KEYWORDS_TO_TYPES().end() &&
            toks[static_cast<size_t>(start + 1)].kind == Token::UQS &&
            toks[static_cast<size_t>(start + 2)].IsDelim('=')) {
            assignmentType = it->second;
            varname = toks[static_cast<size_t>(start + 1)].uqs;
            return true;
        }
    }
    return false;
}

bool RemesParser::StatementIsLoopEnd(const std::vector<Token>& toks, int64_t start, int64_t end) {
    return end <= static_cast<int64_t>(toks.size()) && start == end - 2 &&
           toks[static_cast<size_t>(start)].kind == Token::UQS &&
           toks[static_cast<size_t>(start)].uqs == RemesPathLexer::LOOP_END_KEYWORD &&
           toks[static_cast<size_t>(start + 1)].kind == Token::UQS &&
           toks[static_cast<size_t>(start + 1)].uqs == "for";
}

// ── indexer machinery ──────────────────────────────────────────────────────
// matcher for varname lists: does a key match any of the strings/regexes?
static std::function<bool(const std::string&)> VarnameListMatcher(
    const std::vector<IndexerListValue>& inds) {
    auto keys = std::make_shared<std::unordered_set<std::string>>();
    auto regexes = std::make_shared<std::vector<RegexPtr>>();
    for (const IndexerListValue& v : inds) {
        if (v.kind == IndexerListValue::STRING) keys->insert(v.str);
        else if (v.kind == IndexerListValue::REGEX) regexes->push_back(v.regex);
    }
    if (keys->empty() && regexes->empty())
        throw RemesPathException("Negated indexer with no strings or regexes");
    return [keys, regexes](const std::string& s) {
        if (keys->count(s)) return true;
        for (const RegexPtr& rex : *regexes)
            if (rex->IsMatch(s)) return true;
        return false;
    };
}

// recursive varname-list search (the ".." indexer)
static void RecursiveVarnameSearch(const JNodePtr& x, const std::vector<IndexerListValue>& children,
                                   std::string path,
                                   std::unordered_set<std::string>& pathsVisited,
                                   IndexerRun& run) {
    if (JArray* xarr = AsArr(x)) {
        for (size_t ii = 0; ii < xarr->Length(); ii++)
            RecursiveVarnameSearch(xarr->children[ii], children,
                                   path + "," + std::to_string(ii), pathsVisited, run);
    } else if (JObject* xobj = AsObj(x)) {
        for (const IndexerListValue& v : children) {
            if (v.kind == IndexerListValue::STRING) {
                std::string p = path.empty() ? v.str : path;
                for (auto& kv : xobj->children) {
                    std::string newpath = p + "," + kv.first;
                    if (kv.first == v.str) {
                        if (!pathsVisited.count(newpath)) {
                            IndexerResult r;
                            r.node = kv.second;
                            run.items.push_back(std::move(r));
                        }
                        pathsVisited.insert(newpath);
                    } else {
                        RecursiveVarnameSearch(kv.second, {v}, newpath, pathsVisited, run);
                    }
                }
            } else if (v.kind == IndexerListValue::REGEX) {
                std::string p = path.empty() ? v.regex->Pattern() : path;
                for (auto& kv : xobj->children) {
                    std::string newpath = p + "," + kv.first;
                    if (v.regex->IsMatch(kv.first)) {
                        if (!pathsVisited.count(newpath)) {
                            IndexerResult r;
                            r.node = kv.second;
                            run.items.push_back(std::move(r));
                        }
                        pathsVisited.insert(newpath);
                    } else {
                        RecursiveVarnameSearch(kv.second, {v}, newpath, pathsVisited, run);
                    }
                }
            }
        }
    }
}

// ApplyMultiIndex: select keys (varname list) or indices (slicer list)
static std::function<void(JNodePtr, IndexerRun&)> ApplyMultiIndex(
    std::vector<IndexerListValue> children, bool isVarnameList, bool isRecursive) {
    if (isVarnameList) {
        if (isRecursive) {
            return [children](JNodePtr x, IndexerRun& run) {
                std::unordered_set<std::string> pathsVisited;
                // C# iterates the outer varname list at each object level; the
                // recursion helper above handles one value at a time, so drive
                // the top level here with the full list
                RecursiveVarnameSearch(x, children, "", pathsVisited, run);
            };
        }
        return [children](JNodePtr x, IndexerRun& run) {
            JObject* xobj = AsObj(x);
            if (!xobj) throw RemesPathException("Cannot get keys from a non-object");
            for (const IndexerListValue& v : children) {
                if (v.kind == IndexerListValue::STRING) {
                    JNodePtr val;
                    if (xobj->TryGetValue(v.str, val)) {
                        IndexerResult r;
                        r.isKV = true;
                        r.key = v.str;
                        r.node = val;
                        run.items.push_back(std::move(r));
                    }
                } else if (v.kind == IndexerListValue::REGEX) {
                    for (auto& kv : xobj->children) {
                        if (v.regex->IsMatch(kv.first)) {
                            IndexerResult r;
                            r.isKV = true;
                            r.key = kv.first;
                            r.node = kv.second;
                            run.items.push_back(std::move(r));
                        }
                    }
                }
            }
        };
    }
    if (isRecursive)
        throw RemesPathException("Recursive search for array indices and slices is not implemented");
    return [children](JNodePtr x, IndexerRun& run) {
        JArray* xarr = AsArr(x);
        if (!xarr) throw RemesPathException("Cannot get indices from a non-array");
        int64_t len = static_cast<int64_t>(xarr->Length());
        for (const IndexerListValue& v : children) {
            if (v.kind == IndexerListValue::SLICER) {
                std::vector<int64_t> indices;
                ResolveSlice(len, v.slicer, indices);
                for (int64_t ii : indices) {
                    IndexerResult r;
                    r.node = xarr->children[static_cast<size_t>(ii)];
                    run.items.push_back(std::move(r));
                }
            } else if (v.kind == IndexerListValue::INT) {
                int64_t resolved;
                if (WrappedIndex(len, v.intval, resolved)) {
                    IndexerResult r;
                    r.node = xarr->children[static_cast<size_t>(resolved)];
                    run.items.push_back(std::move(r));
                }
            }
        }
    };
}

static std::function<void(JNodePtr, IndexerRun&)> ApplyNegatedVarnameList(
    std::vector<IndexerListValue> children) {
    auto matcher = VarnameListMatcher(children);
    return [matcher](JNodePtr node, IndexerRun& run) {
        JObject* obj = AsObj(node);
        if (!obj) throw RemesPathException("Cannot get keys from a non-object");
        for (auto& kv : obj->children) {
            if (!matcher(kv.first)) {
                IndexerResult r;
                r.isKV = true;
                r.key = kv.first;
                r.node = kv.second;
                run.items.push_back(std::move(r));
            }
        }
    };
}

static std::function<void(JNodePtr, IndexerRun&)> ApplyNegatedSlicerList(
    std::vector<IndexerListValue> children) {
    return [children](JNodePtr node, IndexerRun& run) {
        JArray* arr = AsArr(node);
        if (!arr) throw RemesPathException("Cannot get indices from a non-array");
        int64_t length = static_cast<int64_t>(arr->Length());
        std::vector<bool> excluded(static_cast<size_t>(length), false);
        for (const IndexerListValue& v : children) {
            if (v.kind == IndexerListValue::INT) {
                int64_t resolved;
                if (WrappedIndex(length, v.intval, resolved))
                    excluded[static_cast<size_t>(resolved)] = true;
            } else if (v.kind == IndexerListValue::SLICER) {
                std::vector<int64_t> indices;
                ResolveSlice(length, v.slicer, indices);
                for (int64_t ii : indices) excluded[static_cast<size_t>(ii)] = true;
            }
        }
        for (int64_t ii = 0; ii < length; ii++) {
            if (!excluded[static_cast<size_t>(ii)]) {
                IndexerResult r;
                r.node = arr->children[static_cast<size_t>(ii)];
                run.items.push_back(std::move(r));
            }
        }
    };
}

static void ApplyStarIndexer(JNodePtr x, IndexerRun& run) {
    if (JObject* xobj = AsObj(x)) {
        for (auto& kv : xobj->children) {
            IndexerResult r;
            r.isKV = true;
            r.key = kv.first;
            r.node = kv.second;
            run.items.push_back(std::move(r));
        }
        return;
    }
    JArray* xarr = AsArr(x);
    if (!xarr) throw RemesPathException("Cannot get keys or indices from a non-iterable");
    for (JNodePtr& child : xarr->children) {
        IndexerResult r;
        r.node = child;
        run.items.push_back(std::move(r));
    }
}

static void RecursivelyFlattenIterable(JNodePtr node, IndexerRun& run) {
    if (JObject* obj = AsObj(node)) {
        for (auto& kv : obj->children) {
            if ((kv.second->type & DT_ITERABLE) == 0) {
                IndexerResult r;
                r.node = kv.second;
                run.items.push_back(std::move(r));
            } else {
                RecursivelyFlattenIterable(kv.second, run);
            }
        }
    } else if (JArray* arr = AsArr(node)) {
        for (JNodePtr& val : arr->children) {
            if ((val->type & DT_ITERABLE) == 0) {
                IndexerResult r;
                r.node = val;
                run.items.push_back(std::move(r));
            } else {
                RecursivelyFlattenIterable(val, run);
            }
        }
    }
}

// boolean index (the only indexer that overrides run.hasOneOption/isDict)
static std::function<void(JNodePtr, IndexerRun&)> ApplyBooleanIndex(JNodePtr inds) {
    return [inds](JNodePtr x, IndexerRun& run) {
        run.hasOneOption = false;
        JNodePtr newinds = inds;
        if (auto cj = std::dynamic_pointer_cast<CurJson>(inds)) newinds = cj->function(x);
        run.isDict = x->type == DT_OBJ;
        if (newinds->type == DT_BOOL) {
            if (newinds->GetBool()) {
                if (JArray* xarr = AsArr(x)) {
                    for (JNodePtr& child : xarr->children) {
                        IndexerResult r;
                        r.node = child;
                        run.items.push_back(std::move(r));
                    }
                } else {
                    run.hasOneOption = true;
                    IndexerResult r;
                    r.node = x;
                    run.items.push_back(std::move(r));
                }
            }
            return;
        }
        if (JObject* iobj = AsObj(newinds)) {
            JObject* xobj = AsObj(x);
            if (!xobj || iobj->Length() != xobj->Length())
                throw VectorizedArithmeticException(
                    "bool index length " + std::to_string(iobj->Length()) +
                    " does not match object/array length " +
                    std::to_string(xobj ? xobj->Length() : 0) + ".");
            for (auto& kv : xobj->children) {
                JNodePtr ival;
                if (iobj->TryGetValue(kv.first, ival)) {
                    if (ival->type != DT_BOOL)
                        throw VectorizedArithmeticException("bool index contains non-booleans");
                    if (ival->GetBool()) {
                        IndexerResult r;
                        r.isKV = true;
                        r.key = kv.first;
                        r.node = kv.second;
                        run.items.push_back(std::move(r));
                    }
                }
            }
            return;
        }
        if (JArray* iarr = AsArr(newinds)) {
            JArray* xarr = AsArr(x);
            if (!xarr || iarr->Length() != xarr->Length())
                throw VectorizedArithmeticException(
                    "bool index length " + std::to_string(iarr->Length()) +
                    " does not match object/array length " +
                    std::to_string(xarr ? xarr->Length() : 0) + ".");
            for (size_t ii = 0; ii < xarr->Length(); ii++) {
                JNodePtr ival = iarr->children[ii];
                if (ival->type != DT_BOOL)
                    throw VectorizedArithmeticException("bool index contains non-booleans");
                if (ival->GetBool()) {
                    IndexerResult r;
                    r.node = xarr->children[ii];
                    run.items.push_back(std::move(r));
                }
            }
            return;
        }
    };
}

// chains a list of indexers into a single function (ApplyIndexerList)
static JNodePtr IdxrListFunc(JNodePtr obj, const std::shared_ptr<std::vector<IndexerFunc>>& idxrs,
                             size_t ii) {
    const IndexerFunc& ix = (*idxrs)[ii];
    IndexerRun run;
    run.hasOneOption = ix.hasOneOption;
    run.isDict = ix.isDict;
    ix.run(obj, run);
    if (run.items.empty()) {
        if (run.isDict) return std::make_shared<JObject>();
        return std::make_shared<JArray>();
    }
    bool isDict = run.items[0].isKV;
    if (ii == idxrs->size() - 1) {
        if (run.hasOneOption) return run.items[0].node;
        if (isDict) {
            auto dic = std::make_shared<JObject>(obj->position);
            for (IndexerResult& r : run.items) dic->children.set(r.key, r.node);
            return dic;
        }
        auto arr = std::make_shared<JArray>(obj->position);
        for (IndexerResult& r : run.items) arr->children.push_back(r.node);
        return arr;
    }
    if (ix.isProjection) {
        if (isDict) {
            auto dic = std::make_shared<JObject>();
            for (IndexerResult& r : run.items) dic->children.set(r.key, r.node);
            return IdxrListFunc(dic, idxrs, ii + 1);
        }
        if (run.hasOneOption) return IdxrListFunc(run.items[0].node, idxrs, ii + 1);
        auto arr = std::make_shared<JArray>();
        for (IndexerResult& r : run.items) arr->children.push_back(r.node);
        return IdxrListFunc(arr, idxrs, ii + 1);
    }
    JNodePtr v1Subdex = IdxrListFunc(run.items[0].node, idxrs, ii + 1);
    if (run.hasOneOption) return v1Subdex;
    int isEmpty = Binop::ObjectOrArrayEmpty(v1Subdex);
    if (isDict) {
        auto dic = std::make_shared<JObject>(obj->position);
        if (isEmpty != 1) dic->children.set(run.items[0].key, v1Subdex);
        for (size_t jj = 1; jj < run.items.size(); jj++) {
            JNodePtr subdex = IdxrListFunc(run.items[jj].node, idxrs, ii + 1);
            if (Binop::ObjectOrArrayEmpty(subdex) != 1)
                dic->children.set(run.items[jj].key, subdex);
        }
        return dic;
    }
    auto arr = std::make_shared<JArray>(obj->position);
    if (isEmpty != 1) arr->children.push_back(v1Subdex);
    for (size_t jj = 1; jj < run.items.size(); jj++) {
        JNodePtr subdex = IdxrListFunc(run.items[jj].node, idxrs, ii + 1);
        if (Binop::ObjectOrArrayEmpty(subdex) != 1) arr->children.push_back(subdex);
    }
    return arr;
}

// ── parse indexers ─────────────────────────────────────────────────────────
std::pair<RemesParser::IndexerStart, int64_t> RemesParser::DetermineIndexerStart(
    const std::vector<Token>& toks, int64_t pos, int64_t end) {
    auto peek = [&](int64_t p) -> const Token* {
        return p + 1 >= end ? nullptr : &toks[static_cast<size_t>(p + 1)];
    };
    const Token* t = peek(pos - 1);
    if (!t || t->kind != Token::DELIM) return {IndexerStart::NOT_AN_INDEXER, pos};
    char d = t->delim;
    static const std::string INDEXER_STARTERS = ".[{>!";
    if (INDEXER_STARTERS.find(d) == std::string::npos)
        return {IndexerStart::NOT_AN_INDEXER, pos};
    const Token* nt = peek(pos);
    if (nt && nt->kind == Token::DELIM) {
        char nd = nt->delim;
        if (d == '.' && nd == '.') {
            const Token* nnt = peek(pos + 1);
            if (nnt && nnt->IsDelim('['))
                return {IndexerStart::DOUBLEDOT_SQUAREBRACE, pos + 3};
            return {IndexerStart::DOUBLEDOT, pos + 2};
        }
        if (d == '!') {
            if (nd == '.') return {IndexerStart::BANG_DOT, pos + 2};
            if (nd == '[') return {IndexerStart::BANG_SQUAREBRACE, pos + 2};
            return {IndexerStart::NOT_AN_INDEXER, pos};
        }
    }
    switch (d) {
        case '[': return {IndexerStart::SQUAREBRACE, pos + 1};
        case '{': return {IndexerStart::CURLYBRACE, pos + 1};
        case '.': return {IndexerStart::DOT, pos + 1};
        case '>': return {IndexerStart::FORWARD_ARROW, pos + 1};
        default: return {IndexerStart::NOT_AN_INDEXER, pos};
    }
}

// single indexer-list value from a parsed JNode
static IndexerListValue GetSingleIndexerListValue(const JNodePtr& ind) {
    IndexerListValue v;
    switch (ind->type) {
        case DT_STR:
            v.kind = IndexerListValue::STRING;
            v.str = ind->GetStr();
            return v;
        case DT_INT:
            v.kind = IndexerListValue::INT;
            v.intval = ind->GetInt();
            return v;
        case DT_SLICE:
            v.kind = IndexerListValue::SLICER;
            v.slicer = static_cast<JSlicer*>(ind.get())->slicer;
            return v;
        case DT_REGEX:
            v.kind = IndexerListValue::REGEX;
            v.regex = static_cast<JRegex*>(ind.get())->regex;
            return v;
        default:
            throw RemesPathException("Entries in an indexer list must be string, regex, int, or slice.");
    }
}

RemesParser::SlicerPos RemesParser::ParseSlicer(std::vector<Token>& toks, int64_t pos,
                                                bool hasFirstNum, int64_t firstNum, int64_t end,
                                                const std::shared_ptr<JQueryContext>& context) {
    Slicer slicer;
    bool lastHas = hasFirstNum;
    int64_t lastNum = firstNum;
    int slotsFilled = 0;
    while (pos < end && slotsFilled < 3) {
        const Token& t = toks[static_cast<size_t>(pos)];
        if (t.kind == Token::DELIM) {
            if (t.delim == ':') {
                slicer.emplace_back(lastHas, lastNum);
                slotsFilled++;
                lastHas = false;
                lastNum = 0;
                pos++;
                continue;
            }
            if (IsExprFuncEnder(t.delim)) break;
        }
        JNodePtr numtok;
        try {
            ObjPosNode npo = ParseExprFunc(toks, pos, end, context);
            numtok = npo.node;
            pos = npo.pos;
            if (numtok->type != DT_INT) throw RemesPathException("non-integer");
        } catch (...) {
            throw RemesPathException("Found non-integer while parsing a slicer");
        }
        lastHas = true;
        lastNum = numtok->GetInt();
        if (slotsFilled == 2) break;
    }
    slicer.emplace_back(lastHas, lastNum);
    return SlicerPos{slicer, pos};
}

RemesParser::ParsedIndexerPos RemesParser::ParseIndexer(
    std::vector<Token>& toks, int64_t pos, int64_t end, IndexerStart indStart,
    const std::shared_ptr<JQueryContext>& context) {
    bool anyDotType = indStart == IndexerStart::DOT || indStart == IndexerStart::DOUBLEDOT ||
                      indStart == IndexerStart::BANG_DOT;
    const Token& t = toks[static_cast<size_t>(pos)];
    if (anyDotType) {
        if (t.kind == Token::BINOP && t.binop->name == "*") {
            ParsedIndexer idx;
            idx.kind = ParsedIndexer::STAR;
            return {idx, pos + 1};
        }
        JNodePtr jt;
        if (t.kind == Token::UQS) jt = JNode::Str(t.uqs);
        else if (t.kind == Token::JNODE) jt = t.node;
        else throw RemesPathException("'.' syntax for indexers requires that the indexer be a string, regex, or '*'");
        if ((jt->type & DT_STR_OR_REGEX) == 0)
            throw RemesPathException("'.' syntax for indexers requires that the indexer be a string, regex, or '*'");
        ParsedIndexer idx;
        idx.kind = ParsedIndexer::VARNAME_LIST;
        IndexerListValue v;
        if (jt->type == DT_REGEX) {
            v.kind = IndexerListValue::REGEX;
            v.regex = static_cast<JRegex*>(jt.get())->regex;
        } else {
            v.kind = IndexerListValue::STRING;
            v.str = jt->GetStr();
        }
        idx.children.push_back(std::move(v));
        return {idx, pos + 1};
    }
    if (indStart == IndexerStart::CURLYBRACE) return ParseProjection(toks, pos, end, context);
    if (indStart == IndexerStart::FORWARD_ARROW) return ParseMap(toks, pos, end, context);
    bool anySquarebraceType = indStart == IndexerStart::BANG_SQUAREBRACE ||
                              indStart == IndexerStart::SQUAREBRACE ||
                              indStart == IndexerStart::DOUBLEDOT_SQUAREBRACE;
    if (!anySquarebraceType)
        throw RemesPathException("Indexer must start with '.', '[', '{', '!', or \"->\"");
    ParsedIndexer indexer;
    bool indexerDetermined = false;
    std::vector<IndexerListValue> children;
    JNodePtr lastTok;
    Dtype lastType = DT_UNKNOWN;
    if (t.kind == Token::BINOP && t.binop->name == "*") {
        // "[*]" star indexer
        if (pos + 1 < end && toks[static_cast<size_t>(pos + 1)].IsDelim(']')) {
            ParsedIndexer idx;
            idx.kind = ParsedIndexer::STAR;
            return {idx, pos + 2};
        }
        throw RemesPathException("Unacceptable first token '*' for indexer list");
    }
    int64_t p = pos;
    while (p < end) {
        const Token& tk = toks[static_cast<size_t>(p)];
        if (tk.kind == Token::DELIM) {
            char d = tk.delim;
            if (d == ']') {
                if (!lastTok) throw RemesPathException("Empty indexer");
                if (!indexerDetermined) {
                    indexerDetermined = true;
                    if ((lastType & DT_STR_OR_REGEX) != 0) indexer.kind = ParsedIndexer::VARNAME_LIST;
                    else if ((lastType & DT_INT_OR_SLICE) != 0) indexer.kind = ParsedIndexer::SLICER_LIST;
                    else {
                        indexer.kind = ParsedIndexer::BOOLEAN;
                        indexer.booleanValue = lastTok;
                    }
                }
                if (indexer.kind == ParsedIndexer::VARNAME_LIST ||
                    indexer.kind == ParsedIndexer::SLICER_LIST) {
                    if ((indexer.kind == ParsedIndexer::VARNAME_LIST && (lastType & DT_STR_OR_REGEX) == 0) ||
                        (indexer.kind == ParsedIndexer::SLICER_LIST && (lastType & DT_INT_OR_SLICE) == 0))
                        throw RemesPathException("Cannot have indexers with a mix of ints/slicers and strings/regexes");
                    children.push_back(GetSingleIndexerListValue(lastTok));
                }
                indexer.children = std::move(children);
                return {indexer, p + 1};
            }
            if (d == ',') {
                if (!lastTok) throw RemesPathException("Comma before first token in indexer");
                if (!indexerDetermined) {
                    indexerDetermined = true;
                    if ((lastType & DT_STR_OR_REGEX) != 0) indexer.kind = ParsedIndexer::VARNAME_LIST;
                    else if ((lastType & DT_INT_OR_SLICE) != 0) indexer.kind = ParsedIndexer::SLICER_LIST;
                    else throw RemesPathException("Cannot have a comma after a boolean index");
                }
                children.push_back(GetSingleIndexerListValue(lastTok));
                lastTok = nullptr;
                lastType = DT_UNKNOWN;
                p++;
            } else if (d == ':') {
                if (!lastTok) {
                    SlicerPos sp = ParseSlicer(toks, p, false, 0, end, context);
                    lastTok = std::make_shared<JSlicer>(sp.slicer);
                    p = sp.pos;
                } else if (lastTok->type == DT_INT) {
                    SlicerPos sp = ParseSlicer(toks, p, true, lastTok->GetInt(), end, context);
                    lastTok = std::make_shared<JSlicer>(sp.slicer);
                    p = sp.pos;
                } else {
                    throw RemesPathException("Expected token other than ':' after " +
                                             lastTok->ToString() + " in an indexer");
                }
                lastType = lastTok->type;
            } else {
                throw RemesPathException("Expected token other than delimiter in an indexer");
            }
        } else if (lastTok) {
            throw RemesPathException("Consecutive indexers must be separated by commas");
        } else {
            ObjPosNode opo = ParseExprFunc(toks, p, end, context);
            lastTok = opo.node;
            p = opo.pos;
            lastType = lastTok->type;
        }
    }
    throw RemesPathException("Unterminated indexer");
}

// ── ParseExpr: parentheses, functions, chained indexers ────────────────────
RemesParser::ObjPosNode RemesParser::ParseExpr(std::vector<Token>& toks, int64_t pos, int64_t end,
                                               const std::shared_ptr<JQueryContext>& context) {
    if (toks.empty()) throw RemesPathException("Empty query");
    const Token& t = toks[static_cast<size_t>(pos)];
    JNodePtr lastTok;
    if (t.kind == Token::BINOP)
        throw RemesPathException("Binop " + t.binop->name + " without appropriate left operand");
    if (t.kind == Token::DELIM) {
        if (t.delim != '(')
            throw RemesPathException(std::string("Invalid token ") + t.delim + " at position " +
                                     std::to_string(pos));
        int unclosedParens = 1;
        int64_t subqueryStart = pos + 1;
        for (int64_t subqueryEnd = subqueryStart; subqueryEnd < end; subqueryEnd++) {
            const Token& subtok = toks[static_cast<size_t>(subqueryEnd)];
            if (subtok.kind == Token::DELIM) {
                if (subtok.delim == '(') unclosedParens++;
                else if (subtok.delim == ')') {
                    if (--unclosedParens == 0) {
                        lastTok = ParseExprFunc(toks, subqueryStart, subqueryEnd, context).node;
                        pos = subqueryEnd + 1;
                        break;
                    }
                }
            }
        }
    } else if (t.kind == Token::UQS) {
        const std::string& s = t.uqs;
        if (pos < end - 1 && toks[static_cast<size_t>(pos + 1)].IsDelim('(')) {
            auto it = ArgFunction::FUNCTIONS().find(s);
            if (it == ArgFunction::FUNCTIONS().end())
                throw RemesPathException("'" + s + "' is not the name of a RemesPath function.");
            ObjPosNode opo = ParseArgFunction(toks, pos + 1, it->second, end, context);
            lastTok = opo.node;
            pos = opo.pos;
        } else {
            lastTok = ParseNonFunctionUnquotedStr(pos, s, context);
            pos++;
        }
    } else {
        lastTok = t.node;
        pos++;
    }
    if (!lastTok) throw RemesPathException("Found null where JNode expected");
    auto [indStart, indStartEndPos] = DetermineIndexerStart(toks, pos, end);
    bool isProjectionStart =
        indStart == IndexerStart::CURLYBRACE || indStart == IndexerStart::FORWARD_ARROW;
    if ((lastTok->type & DT_ITERABLE) != 0 || isProjectionStart) {
        auto idxrs = std::make_shared<std::vector<IndexerFunc>>();
        pos = indStartEndPos;
        Dtype returnType = lastTok->type;
        bool keepChainingReturnType = true;
        while (indStart != IndexerStart::NOT_AN_INDEXER) {
            bool isRecursive = indStart == IndexerStart::DOUBLEDOT ||
                               indStart == IndexerStart::DOUBLEDOT_SQUAREBRACE;
            bool isNegated = indStart == IndexerStart::BANG_DOT ||
                             indStart == IndexerStart::BANG_SQUAREBRACE;
            if (isRecursive && isNegated)
                throw RemesPathException("Recursive negated indexers (of the form \"!..a\" or \"!..[g`a`]\") are not currently supported.");
            ParsedIndexerPos pip = ParseIndexer(toks, pos, end, indStart, context);
            ParsedIndexer& curIdxr = pip.idx;
            bool isVarnameList = curIdxr.kind == ParsedIndexer::VARNAME_LIST;
            bool isDict = isVarnameList && !isRecursive;
            bool hasOneOption = indStart == IndexerStart::FORWARD_ARROW;
            if (isVarnameList || curIdxr.kind == ParsedIndexer::SLICER_LIST) {
                auto& children = curIdxr.children;
                if (isVarnameList) {
                    if (!isRecursive && !isNegated && children.size() == 1 &&
                        children[0].kind == IndexerListValue::STRING)
                        hasOneOption = true;
                } else {
                    if (!isRecursive && !isNegated && children.size() == 1 &&
                        children[0].kind == IndexerListValue::INT)
                        hasOneOption = true;
                }
                IndexerFunc f;
                if (isNegated) {
                    f.run = isVarnameList ? ApplyNegatedVarnameList(children)
                                          : ApplyNegatedSlicerList(children);
                } else {
                    f.run = ApplyMultiIndex(children, isVarnameList, isRecursive);
                }
                f.hasOneOption = hasOneOption;
                f.isProjection = false;
                f.isDict = isDict;
                f.isRecursive = isRecursive;
                idxrs->push_back(std::move(f));
                if (keepChainingReturnType) {
                    if (isRecursive) {
                        keepChainingReturnType = false;
                        returnType = DT_ARR;
                    } else if (hasOneOption && !isNegated) {
                        returnType = DT_UNKNOWN;
                    } else {
                        returnType = isVarnameList ? DT_OBJ : DT_ARR;
                        keepChainingReturnType = false;
                    }
                }
            } else if (curIdxr.kind == ParsedIndexer::BOOLEAN) {
                if (isNegated)
                    throw RemesPathException("Negated boolean indices are not supported; just invert the logic to get the same effect.");
                IndexerFunc f;
                f.run = ApplyBooleanIndex(curIdxr.booleanValue);
                f.hasOneOption = hasOneOption;
                f.isDict = isDict;
                f.isRecursive = isRecursive;
                idxrs->push_back(std::move(f));
                keepChainingReturnType = false;
            } else if (curIdxr.kind == ParsedIndexer::PROJECTION_IDX) {
                if (isNegated) throw RemesPathException("Negated projections are not supported.");
                IndexerFunc f;
                f.run = curIdxr.projFunc;
                f.hasOneOption = hasOneOption;
                f.isProjection = true;
                idxrs->push_back(std::move(f));
                if (keepChainingReturnType) returnType = curIdxr.projReturnType;
            } else { // star indexer
                if (isNegated) throw RemesPathException("Negated star indexers are not supported.");
                IndexerFunc f;
                if (isRecursive) {
                    if (keepChainingReturnType) returnType = DT_ARR;
                    f.run = [](JNodePtr x, IndexerRun& run) { RecursivelyFlattenIterable(x, run); };
                    f.isRecursive = true;
                } else {
                    f.run = ApplyStarIndexer;
                    f.hasOneOption = hasOneOption;
                    f.isDict = isDict;
                }
                idxrs->push_back(std::move(f));
                keepChainingReturnType = false;
            }
            auto next = DetermineIndexerStart(toks, pip.pos, end);
            indStart = next.first;
            pos = next.second;
            if (indStart == IndexerStart::NOT_AN_INDEXER) pos = pip.pos;
        }
        if (!idxrs->empty()) {
            auto idxrsFunc = [idxrs](JNodePtr x) { return IdxrListFunc(std::move(x), idxrs, 0); };
            if (auto lcur = std::dynamic_pointer_cast<CurJson>(lastTok)) {
                return {std::make_shared<CurJson>(returnType, [idxrsFunc, lcur](JNodePtr inp) {
                            return idxrsFunc(lcur->function(inp));
                        }),
                        pos};
            }
            if (context->AnyVariableReferencedInRange(indStartEndPos, pos)) {
                return {std::make_shared<CurJson>(returnType, [idxrsFunc, lastTok](JNodePtr) {
                            return idxrsFunc(lastTok);
                        }),
                        pos};
            }
            return {idxrsFunc(lastTok), pos};
        }
    }
    return {lastTok, pos};
}

JNodePtr RemesParser::ParseNonFunctionUnquotedStr(int64_t tokenIndex, const std::string& uqs,
                                                  const std::shared_ptr<JQueryContext>& context) {
    JNodePtr varNamedS;
    if (context->TryGetValue(tokenIndex, uqs, varNamedS)) return varNamedS;
    return JNode::Str(uqs);
}

RemesParser::ObjPosNode RemesParser::ParseExprFunc(std::vector<Token>& toks, int64_t pos,
                                                   int64_t end,
                                                   const std::shared_ptr<JQueryContext>& context) {
    auto peek = [&](int64_t p) -> const Token* {
        return p + 1 >= end ? nullptr : &toks[static_cast<size_t>(p + 1)];
    };
    const Token* nt = peek(pos);
    // common case: a single JNode followed by the end or an expr-func ender
    if (!nt || (nt->kind == Token::DELIM && IsExprFuncEnder(nt->delim))) {
        const Token& curtok = toks[static_cast<size_t>(pos)];
        JNodePtr node;
        if (curtok.kind == Token::UQS)
            node = ParseNonFunctionUnquotedStr(pos, curtok.uqs, context);
        else if (curtok.kind == Token::JNODE)
            node = curtok.node;
        else
            throw RemesPathException("Invalid token where JNode expected");
        return {node, pos + 1};
    }
    bool curtokIsNode = false;
    JNodePtr curtokNode;
    bool leftTokIsNode;
    std::vector<UnaryOpPtr> unopStack;
    std::vector<BinopWithArgsPtr> bwaStack;
    std::vector<ArgStackEntry> argStack;
    ArgStackEntry leftOperand;
    while (pos < end) {
        leftTokIsNode = curtokIsNode;
        JNodePtr leftTokNode = curtokNode;
        const Token& curtok = toks[static_cast<size_t>(pos)];
        curtokIsNode = false;
        curtokNode = nullptr;
        if (curtok.kind == Token::DELIM && IsExprFuncEnder(curtok.delim)) {
            if (!leftTokIsNode)
                throw RemesPathException("No expression found where scalar expected");
            break;
        }
        if (curtok.kind == Token::BINOP) {
            BinopPtr bop = curtok.binop;
            if (!leftTokIsNode) {
                // maybe this binop is actually a unary operator with the same name
                auto uit = UnaryOp::UNARY_OPS().find(bop->name);
                if (uit != UnaryOp::UNARY_OPS().end())
                    uit->second->AddToStack(unopStack, uit->second);
                else
                    throw RemesPathException("Binop " + bop->name + " with invalid left operand");
            } else {
                std::vector<JNodePtr> leftOperandTransformations;
                std::vector<BinopPtr> binopTransformations;
                if (!unopStack.empty()) {
                    leftOperandTransformations.push_back(leftTokNode);
                    binopTransformations.push_back(bop);
                }
                while (!unopStack.empty()) {
                    UnaryOpPtr unop = unopStack.back();
                    unopStack.pop_back();
                    if (bop->PrecedesLeft(unop->precedence)) {
                        BinopPtr oldBop = binopTransformations.back();
                        auto oldBopCall = [oldBop](JNodePtr a, JNodePtr b) { return oldBop->Call(a, b); };
                        auto unopCall = [unop](JNodePtr x) { return unop->Call(std::move(x)); };
                        binopTransformations.push_back(std::make_shared<Binop>(
                            [unopCall, oldBopCall](JNodePtr a, JNodePtr b) {
                                return unopCall(oldBopCall(std::move(a), std::move(b)));
                            },
                            oldBop->precedence, oldBop->name, oldBop->isRightAssociative));
                    } else {
                        JNodePtr oldLeftOperand = leftOperandTransformations.back();
                        leftOperandTransformations.push_back(unop->Call(oldLeftOperand));
                    }
                }
                if (!leftOperandTransformations.empty())
                    argStack.back() = ArgStackEntry{leftOperandTransformations.back(), nullptr};
                if (!binopTransformations.empty()) bop = binopTransformations.back();
                bwaStack.push_back(std::make_shared<BinopWithArgs>(bop, ArgStackEntry{}, ArgStackEntry{}));
                leftOperand = BinopWithArgs::ResolveStack(bwaStack, argStack);
            }
            pos++;
        } else if (curtok.kind == Token::UNOP) {
            curtok.unop->AddToStack(unopStack, curtok.unop);
            pos++;
        } else {
            ObjPosNode opo = ParseExpr(toks, pos, end, context);
            pos = opo.pos;
            JNodePtr onode = opo.node;
            if (leftTokIsNode)
                throw RemesPathException("Two JNodes " + leftTokNode->ToString() + " and " +
                                         onode->ToString() + " with no binop in between");
            const Token* nt2 = peek(pos - 1);
            if (!nt2 || (nt2->kind == Token::DELIM && IsExprFuncEnder(nt2->delim))) {
                // no more binops to fight with; apply all unary ops
                while (!unopStack.empty()) {
                    UnaryOpPtr unop = unopStack.back();
                    unopStack.pop_back();
                    onode = unop->Call(onode);
                }
            }
            argStack.push_back(ArgStackEntry{onode, nullptr});
            // no binop coming up, so clean up the stack
            if (pos >= end ||
                !(toks[static_cast<size_t>(pos)].kind == Token::BINOP && !bwaStack.empty()))
                leftOperand = BinopWithArgs::ResolveStack(bwaStack, argStack);
            curtokIsNode = true;
            curtokNode = onode;
        }
    }
    if (leftOperand.empty()) throw RemesPathException("Null return from ParseExprFunc");
    if (leftOperand.bwa) throw RemesPathException("Binop " + leftOperand.bwa->binop->name + " with missing operand");
    return {leftOperand.node, pos};
}

// ── ApplyArgFunction (compile-time partial evaluation) ─────────────────────
static JNodePtr CallVectorizedArgFuncWithArgs(const JNodePtr& x,
                                              const std::vector<JNodePtr>& otherArgs,
                                              const std::shared_ptr<std::vector<JNodePtr>>& allArgs,
                                              const ArgFunctionPtr& func) {
    for (size_t ii = 0; ii < otherArgs.size(); ii++) (*allArgs)[ii + 1] = otherArgs[ii];
    if (JObject* xobj = AsObj(x)) {
        auto dic = std::make_shared<JObject>();
        for (auto& xkv : xobj->children) {
            (*allArgs)[0] = xkv.second;
            dic->children.set(xkv.first, func->Call(*allArgs));
        }
        return dic;
    }
    if (JArray* xarr = AsArr(x)) {
        auto arr = std::make_shared<JArray>();
        for (JNodePtr& val : xarr->children) {
            (*allArgs)[0] = val;
            arr->children.push_back(func->Call(*allArgs));
        }
        return arr;
    }
    (*allArgs)[0] = x;
    return func->Call(*allArgs);
}

JNodePtr RemesParser::ApplyArgFunction(ArgFunctionWithArgs& func) {
    ArgFunctionPtr fn = func.function;
    if (fn->conditionalExecution) {
        // add a CurJson after the normal args so '@' in deferred args resolves
        func.args.push_back(std::make_shared<CurJson>());
    }
    if (fn->maxArgs == 0) {
        auto argsCopy = std::make_shared<std::vector<JNodePtr>>(func.args);
        if (!fn->isDeterministic)
            return std::make_shared<CurJson>(fn->type,
                                             [fn, argsCopy](JNodePtr) { return fn->Call(*argsCopy); });
        return fn->Call(func.args);
    }
    if (fn->argsTransform) fn->argsTransform->Transform(func.args);
    JNodePtr x = func.args[0];
    bool otherCallables = false;
    std::vector<JNodePtr> otherArgs;
    std::vector<bool> argsCanBeFunctions(func.args.size() - 1, false);
    bool firstArgCanBeFunction = (fn->TypeOptions(0) & DT_FUNCTION) != 0;
    for (size_t ii = 0; ii + 1 < func.args.size(); ii++) {
        JNodePtr arg = func.args[ii + 1];
        if (std::dynamic_pointer_cast<CurJson>(arg)) otherCallables = true;
        argsCanBeFunctions[ii] = (fn->TypeOptions(static_cast<int64_t>(ii + 1)) & DT_FUNCTION) != 0;
        otherArgs.push_back(arg);
    }
    if (fn->conditionalExecution && !argsCanBeFunctions.empty())
        argsCanBeFunctions[argsCanBeFunctions.size() - 1] = false;
    Dtype outType = fn->OutputType(x);
    auto allArgs = std::make_shared<std::vector<JNodePtr>>(func.args.size(), nullptr);
    auto fillOtherArgs = [otherArgs, argsCanBeFunctions, allArgs](JNodePtr inp) {
        for (size_t ii = 0; ii < otherArgs.size(); ii++) {
            JNodePtr otherArg = otherArgs[ii];
            auto cjoa = std::dynamic_pointer_cast<CurJson>(otherArg);
            (*allArgs)[ii + 1] = (cjoa && !argsCanBeFunctions[ii]) ? cjoa->function(inp) : otherArg;
        }
    };
    if (fn->isVectorized) {
        auto xcur = std::dynamic_pointer_cast<CurJson>(x);
        if (xcur) {
            auto argOutfunc = [fn, xcur, fillOtherArgs, allArgs, firstArgCanBeFunction,
                               otherCallables, otherArgs](JNodePtr inp) -> JNodePtr {
                if (otherCallables) fillOtherArgs(inp);
                else
                    for (size_t ii = 0; ii < otherArgs.size(); ii++) (*allArgs)[ii + 1] = otherArgs[ii];
                JNodePtr itbl = firstArgCanBeFunction ? std::static_pointer_cast<JNode>(xcur)
                                                      : xcur->function(inp);
                if (JObject* otbl = AsObj(itbl)) {
                    auto dic = std::make_shared<JObject>();
                    for (auto& okv : otbl->children) {
                        (*allArgs)[0] = okv.second;
                        dic->children.set(okv.first, fn->Call(*allArgs));
                    }
                    return dic;
                }
                if (JArray* atbl = AsArr(itbl)) {
                    auto arr = std::make_shared<JArray>();
                    for (JNodePtr& val : atbl->children) {
                        (*allArgs)[0] = val;
                        arr->children.push_back(fn->Call(*allArgs));
                    }
                    return arr;
                }
                (*allArgs)[0] = itbl;
                return fn->Call(*allArgs);
            };
            return std::make_shared<CurJson>(outType, argOutfunc);
        }
        if (otherCallables) {
            auto argOutfunc = [fn, x, fillOtherArgs, allArgs](JNodePtr inp) -> JNodePtr {
                fillOtherArgs(inp);
                if (JObject* xobj = AsObj(x)) {
                    auto dic = std::make_shared<JObject>();
                    for (auto& xkv : xobj->children) {
                        (*allArgs)[0] = xkv.second;
                        dic->children.set(xkv.first, fn->Call(*allArgs));
                    }
                    return dic;
                }
                if (JArray* xarr = AsArr(x)) {
                    auto arr = std::make_shared<JArray>();
                    for (JNodePtr& val : xarr->children) {
                        (*allArgs)[0] = val;
                        arr->children.push_back(fn->Call(*allArgs));
                    }
                    return arr;
                }
                (*allArgs)[0] = x;
                return fn->Call(*allArgs);
            };
            return std::make_shared<CurJson>(outType, argOutfunc);
        }
        if (!fn->isDeterministic)
            return std::make_shared<CurJson>(fn->type, [x, otherArgs, allArgs, fn](JNodePtr) {
                return CallVectorizedArgFuncWithArgs(x, otherArgs, allArgs, fn);
            });
        return CallVectorizedArgFuncWithArgs(x, otherArgs, allArgs, fn);
    }
    // non-vectorized
    auto xcur = std::dynamic_pointer_cast<CurJson>(x);
    if (xcur) {
        auto argOutfunc = [fn, xcur, fillOtherArgs, allArgs, firstArgCanBeFunction, otherCallables,
                           otherArgs](JNodePtr inp) -> JNodePtr {
            if (otherCallables) fillOtherArgs(inp);
            else
                for (size_t ii = 0; ii < otherArgs.size(); ii++) (*allArgs)[ii + 1] = otherArgs[ii];
            (*allArgs)[0] = firstArgCanBeFunction ? std::static_pointer_cast<JNode>(xcur)
                                                  : xcur->function(inp);
            return fn->Call(*allArgs);
        };
        return std::make_shared<CurJson>(outType, argOutfunc);
    }
    if (otherCallables) {
        auto argOutfunc = [fn, x, fillOtherArgs, allArgs](JNodePtr inp) -> JNodePtr {
            fillOtherArgs(inp);
            (*allArgs)[0] = x;
            return fn->Call(*allArgs);
        };
        return std::make_shared<CurJson>(outType, argOutfunc);
    }
    for (size_t ii = 0; ii < otherArgs.size(); ii++) (*allArgs)[ii + 1] = otherArgs[ii];
    (*allArgs)[0] = x;
    if (!fn->isDeterministic)
        return std::make_shared<CurJson>(fn->type,
                                         [fn, allArgs](JNodePtr) { return fn->Call(*allArgs); });
    return fn->Call(*allArgs);
}

RemesParser::ObjPosNode RemesParser::ParseArgFunction(std::vector<Token>& toks, int64_t pos,
                                                      const ArgFunctionPtr& fun, int64_t end,
                                                      const std::shared_ptr<JQueryContext>& context) {
    pos++;
    int64_t argNum = 0;
    std::vector<JNodePtr> args;
    if (fun->maxArgs == 0) {
        if (!(pos < end && toks[static_cast<size_t>(pos)].IsDelim(')')))
            throw RemesPathException("Expected no arguments for function " + fun->name + " (0 args)");
        ArgFunctionWithArgs withArgs(fun, args);
        return {ApplyArgFunction(withArgs), pos + 1};
    }
    JNodePtr curArg;
    while (pos < end) {
        const Token& t = toks[static_cast<size_t>(pos)];
        if (t.kind == Token::DELIM && (t.delim == ',' || t.delim == ')')) {
            char d = t.delim;
            if (!(fun->maxArgs > fun->minArgs && argNum >= fun->minArgs &&
                  ((d == ',' && argNum < fun->maxArgs - 1) || d == ')')))
                throw RemesPathArgumentException(
                    "Omitting a required argument for a function is not allowed", argNum, fun.get());
            args.push_back(std::make_shared<JNode>());
            argNum++;
            pos++;
            if (d == ')') {
                ArgFunctionWithArgs withargs(fun, args);
                fun->PadToMaxArgs(withargs.args);
                return {ApplyArgFunction(withargs), pos};
            }
            continue;
        }
        Dtype typeOptions = fun->TypeOptions(argNum);
        bool spreadCurArg = t.kind == Token::BINOP && t.binop->name == "*";
        if (spreadCurArg) pos++;
        try {
            try {
                ObjPosNode opo = ParseExprFunc(toks, pos, end, context);
                curArg = opo.node;
                pos = opo.pos;
            } catch (...) {
                curArg = nullptr;
            }
            if ((DT_SLICE & typeOptions) != 0) {
                const Token* nt = pos - 1 + 1 < end ? &toks[static_cast<size_t>(pos)] : nullptr;
                if (nt && nt->IsDelim(':')) {
                    bool hasFirst = curArg != nullptr;
                    int64_t firstNum = hasFirst ? curArg->GetInt() : 0;
                    SlicerPos sp = ParseSlicer(toks, pos, hasFirst, firstNum, end, context);
                    curArg = std::make_shared<JSlicer>(sp.slicer);
                    pos = sp.pos;
                }
            }
            if (!spreadCurArg) fun->CheckType(curArg, argNum);
        } catch (const RemesPathArgumentException&) {
            throw;
        } catch (const std::exception& ex) {
            throw RemesPathArgumentException(std::string("threw exception ") + ex.what(), argNum,
                                             fun.get());
        }
        if (pos >= end) break;
        const Token& t2 = toks[static_cast<size_t>(pos)];
        pos++;
        bool comma = false, closeParen = false;
        if (t2.kind == Token::DELIM) {
            comma = t2.delim == ',';
            closeParen = t2.delim == ')';
        } else {
            throw RemesPathException("Arguments of arg functions must be followed by ',' or ')'");
        }
        if (spreadCurArg) {
            if (!closeParen)
                throw RemesPathException("There can be no arguments to a function after an array that was spread to multiple args using the '*' operator");
            JNodePtr spreadResult = SpreadArrayToArgFunctionArgs(args, curArg, fun);
            return {spreadResult, pos};
        }
        args.push_back(curArg);
        argNum++;
        if ((argNum < fun->minArgs && !comma) || (argNum == fun->maxArgs && !closeParen))
            fun->ThrowWrongArgCount(argNum);
        if (closeParen) {
            ArgFunctionWithArgs withargs(fun, args);
            fun->PadToMaxArgs(withargs.args);
            return {ApplyArgFunction(withargs), pos};
        }
    }
    fun->ThrowWrongArgCount(argNum);
}

JNodePtr RemesParser::SpreadArrayToArgFunctionArgs(const std::vector<JNodePtr>& args,
                                                   JNodePtr curArg, const ArgFunctionPtr& fun) {
    if (auto cj = std::dynamic_pointer_cast<CurJson>(curArg)) {
        RemesParser* self = this;
        std::vector<JNodePtr> argsCopy = args;
        auto spreadFun = [self, cj, argsCopy, fun](JNodePtr inp) -> JNodePtr {
            JNodePtr inpArr = cj->function(inp);
            std::vector<JNodePtr> argsCalledOnInp;
            for (const JNodePtr& x : argsCopy) {
                auto xcj = std::dynamic_pointer_cast<CurJson>(x);
                argsCalledOnInp.push_back(xcj ? xcj->function(inp) : x);
            }
            return self->SpreadArrayToArgFunctionArgs(argsCalledOnInp, inpArr, fun);
        };
        return std::make_shared<CurJson>(fun->OutputType(curArg), spreadFun);
    }
    std::vector<JNodePtr> argsCpy = args;
    JArray* curArr = AsArr(curArg);
    if (!curArr)
        throw RemesPathException("Any function argument preceded by '*' must be an array, got type " +
                                 FormatDtype(curArg->type));
    int64_t argNum = static_cast<int64_t>(argsCpy.size());
    for (JNodePtr& child : curArr->children) {
        fun->CheckType(child, argNum);
        argsCpy.push_back(child);
        argNum++;
    }
    if (argNum < fun->minArgs || argNum > fun->maxArgs) fun->ThrowWrongArgCount(argNum);
    fun->PadToMaxArgs(argsCpy);
    ArgFunctionWithArgs withargs(fun, argsCpy);
    return ApplyArgFunction(withargs);
}

RemesParser::ParsedIndexerPos RemesParser::ParseProjection(
    std::vector<Token>& toks, int64_t pos, int64_t end,
    const std::shared_ptr<JQueryContext>& context) {
    struct ProjChild {
        bool isKV = false;
        std::string key;
        JNodePtr value;
    };
    auto children = std::make_shared<std::vector<ProjChild>>();
    bool isObjectProj = false;
    while (pos < end) {
        ObjPosNode opo = ParseExprFunc(toks, pos, end, context);
        JNodePtr key = opo.node;
        pos = opo.pos;
        const Token* nt = pos - 1 + 1 < end ? &toks[static_cast<size_t>(pos)] : nullptr;
        if (!nt || nt->kind != Token::DELIM)
            throw RemesPathException("Values or key-value pairs in a projection must be comma-delimited");
        char nd = nt->delim;
        if (nd == ':') {
            if (!children->empty() && !isObjectProj)
                throw RemesPathException("Mixture of values and key-value pairs in object/array projection");
            if (!key->IsStrValue())
                throw RemesPathException("Object projection keys must be string, not type " +
                                         FormatDtype(key->type) + " (value " + key->ToString() + ")");
            std::string keystr = key->GetStr();
            opo = ParseExprFunc(toks, pos + 1, end, context);
            JNodePtr val = opo.node;
            pos = opo.pos;
            ProjChild pc;
            pc.isKV = true;
            pc.key = keystr;
            pc.value = val;
            children->push_back(std::move(pc));
            isObjectProj = true;
            nt = pos < end ? &toks[static_cast<size_t>(pos)] : nullptr;
            if (!nt || nt->kind != Token::DELIM)
                throw RemesPathException("Key-value pairs in projection must be delimited by ',' and projections must end with '}'.");
            nd = nt->delim;
        } else {
            ProjChild pc;
            pc.value = key;
            children->push_back(std::move(pc));
        }
        if (nd == '}') {
            ParsedIndexer idx;
            idx.kind = ParsedIndexer::PROJECTION_IDX;
            if (isObjectProj) {
                idx.projReturnType = DT_OBJ;
                idx.projFunc = [children](JNodePtr obj, IndexerRun& run) {
                    for (const ProjChild& child : *children) {
                        IndexerResult r;
                        r.isKV = true;
                        r.key = child.key;
                        auto cj = std::dynamic_pointer_cast<CurJson>(child.value);
                        r.node = cj ? cj->function(obj) : child.value->Copy();
                        run.items.push_back(std::move(r));
                    }
                };
            } else {
                idx.projReturnType = DT_ARR;
                idx.projFunc = [children](JNodePtr obj, IndexerRun& run) {
                    for (const ProjChild& child : *children) {
                        IndexerResult r;
                        auto cj = std::dynamic_pointer_cast<CurJson>(child.value);
                        r.node = cj ? cj->function(obj) : child.value->Copy();
                        run.items.push_back(std::move(r));
                    }
                };
            }
            return {idx, pos + 1};
        }
        if (nd != ',')
            throw RemesPathException("Values or key-value pairs in a projection must be comma-delimited");
        pos++;
    }
    throw RemesPathException("Unterminated projection");
}

RemesParser::ParsedIndexerPos RemesParser::ParseMap(std::vector<Token>& toks, int64_t pos,
                                                    int64_t end,
                                                    const std::shared_ptr<JQueryContext>& context) {
    ObjPosNode opo = ParseExprFunc(toks, pos, end, context);
    JNodePtr val = opo.node;
    pos = opo.pos;
    std::function<JNodePtr(JNodePtr)> outfunc;
    if (auto cj = std::dynamic_pointer_cast<CurJson>(val)) outfunc = cj->function;
    else outfunc = [val](JNodePtr) { return val->Copy(); };
    ParsedIndexer idx;
    idx.kind = ParsedIndexer::PROJECTION_IDX;
    idx.projReturnType = val->type;
    idx.projFunc = [outfunc](JNodePtr x, IndexerRun& run) {
        IndexerResult r;
        r.node = outfunc(std::move(x));
        run.items.push_back(std::move(r));
    };
    return {idx, pos};
}

std::string RemesParser::PrettifyException(const std::exception& ex) {
    return ex.what();
}

} // namespace jsontools
