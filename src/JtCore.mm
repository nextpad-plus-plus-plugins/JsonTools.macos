// JsonTools.macos — JtCore.mm: see JtCore.h. Port of Main.cs.

#import <Cocoa/Cocoa.h>

#include "JtCore.h"

#include <cfloat>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

#include "JsonSchemaMaker.h"
#include "JsonSchemaValidator.h"
#include "MiscModules.h"
#include "RandomJsonFromSchema.h"
#include "RemesPath.h"

#include "GrepperForm.h"
#include "JtForms.h"
#include "JtNpp.h"
#include "JtSettings.h"
#include "TreeViewer.h"

using namespace jsontools;
using namespace jtnpp;

namespace jtcore {

static const char* PLUGIN_REPO_URL = "https://github.com/molsonkiko/JsonToolsNppPlugin";
static const char* FILE_EXTENSIONS_TO_AUTO_PARSE[] = {"json", "jsonc", "jsonl", "json5"};

std::string activeFname;
std::map<std::string, JsonFileInfo> jsonFileInfos;
JTTreeViewer* openTreeViewer = nil;
char pathSeparator = JNode::DEFAULT_PATH_SEPARATOR;
bool errorFormTriggeredParse = false;
bool grepperTreeViewJustOpened = false;

static IniFileParser sIniParser;
static RemesParser sRemesParser;
static YamlDumper sYamlDumper;
static std::map<uintptr_t, std::string> sJsonFilesRenamed;
// bufferId → fname, maintained on NPPN_BUFFERACTIVATED: NPPN_FILECLOSED
// arrives after the host can no longer resolve the id to a path (and buffer
// ids must never be retained past that notification — pointer use only)
static std::map<uintptr_t, std::string> sBufferIdToFname;
static bool sShouldRenameGrepperForm = false;
static bool sHasWarnedSelectionsForgotten = false;
static bool sBufferFinishedOpening = false;
static double sLastEditedTime = DBL_MAX; // CFAbsoluteTime; DBL_MAX = "nothing pending"

// schema auto-validation
static std::string sSchemasToFnamePatternsFname;
static std::shared_ptr<JObject> sSchemasToFnamePatterns = std::make_shared<JObject>();

static bool IsAutoParseExtension(const std::string& ext) {
    for (const char* e : FILE_EXTENSIONS_TO_AUTO_PARSE)
        if (ext == e) return true;
    return false;
}

static DocumentType DocumentTypeFromFileExtension(const std::string& ext) {
    if (ext == "json" || ext == "json5" || ext == "jsonc") return DocumentType::JSON;
    if (ext == "jsonl") return DocumentType::JSONL;
    if (ext == "ini") return DocumentType::INI;
    return DocumentType::NONE;
}

static const char* DocumentTypeSuperTypeName(DocumentType dt) {
    return dt == DocumentType::INI ? "INI file" : "JSON";
}

bool TryGetInfoForFile(const std::string& fname, JsonFileInfo** info) {
    auto it = jsonFileInfos.find(fname);
    if (it == jsonFileInfos.end()) return false;
    *info = &it->second;
    return true;
}

JsonFileInfo& AddJsonForFile(const std::string& fname, const JNodePtr& json) {
    JsonFileInfo& info = jsonFileInfos[fname];
    info.json = json;
    if (info.tv) [info.tv setJson:json];
    return info;
}

static void DisposeInfo(JsonFileInfo& info) {
    if (info.tv) {
        [info.tv closePanel];
        info.tv = nil;
    }
    info.json = nullptr;
    info.lints.clear();
    info.hasLints = false;
    info.comments.clear();
    info.hasComments = false;
    info.statusBarSection.clear();
}

JsonParser JsonParserFromSettings() {
    const JtSettings& s = jtSettings();
    return JsonParser(s.logger_level, false, false, s.remember_comments);
}

std::string PrettyPrintFromSettings(const JNodePtr& json) {
    const JtSettings& s = jtSettings();
    char indentChar = s.tab_indent_pretty_print ? '\t' : ' ';
    int indent = s.tab_indent_pretty_print ? 1 : s.indent_pretty_print;
    return json->PrettyPrintAndChangePositions(indent, s.sort_keys, s.pretty_print_style,
                                               INT64_MAX, indentChar);
}

std::string ToJsonLinesFromSettings(const JNodePtr& json) {
    const JtSettings& s = jtSettings();
    JArray* arr = AsArr(json);
    if (s.minimal_whitespace_compression) return arr->ToJsonLines(s.sort_keys, ":", ",");
    return arr->ToJsonLines(s.sort_keys);
}

static bool UseComments(const JsonFileInfo& info) {
    return jtSettings().remember_comments && !info.usesSelections && info.hasComments &&
           !info.comments.empty();
}

// upstream Npp.RemoveTrailingSOH: Notepad++ sometimes leaves a trailing \x01
// after SetText; harmless to check here too
static void RemoveTrailingSOH() {
    int64_t len = DocLength();
    if (len > 0 && GetSlice(len - 1, len) == "\x01") DeleteRange(len - 1, 1);
}

static void SetLangBasedOnDocType(bool fatalError, bool usesSelections, DocumentType documentType) {
    if (fatalError || usesSelections) return;
    if (DocLength() >= jtSettings().max_file_size_MB_slow_actions * 1e6) return;
    switch (documentType) {
        case DocumentType::JSON:
        case DocumentType::JSONL: SetCurrentLang(L_JSON); break;
        case DocumentType::INI: SetCurrentLang(L_INI); break;
        default: break;
    }
}

// ── selection remembering helpers ────────────────────────────────────────────
static int64_t ApplyAndSwapIndicator(int64_t currentIndicator, int64_t start, int64_t length) {
    SetIndicatorCurrent(currentIndicator);
    IndicatorFillRange(start, length);
    return currentIndicator == gSelRememberIndicator1 ? gSelRememberIndicator2
                                                      : gSelRememberIndicator1;
}

static void ClearPreviouslyRememberedSelections(int64_t len, size_t numSelections) {
    SetIndicatorCurrent(gSelRememberIndicator1);
    IndicatorClearRange(0, len);
    if (numSelections > 1) {
        SetIndicatorCurrent(gSelRememberIndicator2);
        IndicatorClearRange(0, len);
        SetIndicatorCurrent(gSelRememberIndicator1);
    }
}

// upstream SelectionManager.NoSelectionIsValidJson: a single selection where
// every parse was fatal probably means the user was just selecting text to copy
static bool NoSelectionIsValidJson(const JNodePtr& json, bool hadSelections,
                                   const std::vector<bool>& fatalErrors) {
    if (!hadSelections || fatalErrors.empty()) return false;
    for (bool fatal : fatalErrors)
        if (!fatal) return false;
    (void)json;
    return true;
}

static void SetStatusBarSectionText(ParserState parserState, const std::string& fname,
                                    JsonFileInfo& info, DocumentType documentType,
                                    size_t lintCount) {
    std::string jsonType = documentType == DocumentType::JSONL ? "JSON lines" : "JSON";
    if (parserState == ParserState::FATAL) {
        std::string ext = FileExtension(fname);
        if (IsAutoParseExtension(ext))
            SetStatusBarSection(jsonType + " with fatal errors - " + std::to_string(lintCount) +
                                " errors (JsonTools > See most recent syntax errors)");
        return;
    }
    if (!info.usesSelections && documentType != DocumentType::JSON &&
        documentType != DocumentType::JSONL)
        return;
    std::string doctypeDescription;
    switch (parserState) {
        case ParserState::STRICT: doctypeDescription = jsonType; break;
        case ParserState::OK: doctypeDescription = jsonType + " (w/ control chars in strings)"; break;
        case ParserState::NAN_INF: doctypeDescription = jsonType + " (w/ NaN and/or Infinity)"; break;
        case ParserState::JSONC: doctypeDescription = jsonType + " with comments"; break;
        case ParserState::JSON5:
            doctypeDescription = documentType == DocumentType::JSONL ? "JSON5 lines" : "JSON5";
            break;
        case ParserState::BAD: doctypeDescription = "Non-compliant " + jsonType; break;
        default: return;
    }
    std::string entry = lintCount == 0
        ? doctypeDescription
        : doctypeDescription + " - " + std::to_string(lintCount) + " errors";
    info.statusBarSection = entry;
    SetStatusBarSection(entry);
}

static std::string HandleJsonParserError(const std::string& text, std::vector<bool>& fatalErrors,
                                         std::vector<JsonLint>& lints, JsonParser& jsonParser,
                                         const std::exception& ex) {
    fatalErrors.push_back(true);
    char errorChar;
    int64_t errorPos;
    if (jsonParser.ii >= (int64_t)text.size()) {
        errorChar = '\x00';
        errorPos = (int64_t)text.size();
    } else {
        errorPos = jsonParser.ii;
        errorChar = text[(size_t)errorPos];
    }
    std::string errorMessage = RemesParser::PrettifyException(ex);
    lints.emplace_back(JsonLintType::FATAL_UNSPECIFIED_ERROR, errorPos, errorChar, errorMessage);
    return errorMessage;
}

ParseResult TryParseJson(DocumentType documentType, bool wasAutotriggered,
                         bool preferPreviousDocumentType, bool isRecursion,
                         bool ignoreSelections) {
    int64_t len = DocLength();
    JsonParser jsonParser = JsonParserFromSettings();
    std::string fname = GetCurrentFilePath();
    // keep activeFname in sync even when no NPPN_BUFFERACTIVATED reached us
    // yet: at startup the session's tabs are restored BEFORE plugins load, so
    // the first command of the session (incl. the host's panel-restore replay
    // of "Open JSON tree viewer") would otherwise look up info under "" and
    // silently no-op
    activeFname = fname;
    auto selRanges = GetSelectedRanges();
    std::vector<std::string> oldSelections;
    auto firstSel = selRanges[0];
    int64_t firstSelLen = firstSel.second - firstSel.first;
    bool oneSelRange = selRanges.size() == 1;
    JTGrepperForm* grepperForm = [JTGrepperForm shared];
    bool noTextSelected =
        (oneSelRange && firstSelLen == 0) ||
        (grepperForm && [grepperForm tv] && activeFname == [[grepperForm tv] fname]) ||
        ignoreSelections;
    bool stopUsingSelections = false;
    double sizeThreshold = jtSettings().max_file_size_MB_slow_actions * 1e6;
    bool hasOldSelections = false;
    DocumentType previouslyChosenDocType = DocumentType::NONE;
    JsonFileInfo* existingInfo = nullptr;
    if (TryGetInfoForFile(fname, &existingInfo)) {
        previouslyChosenDocType = existingInfo->documentType;
        if (existingInfo->usesSelections) {
            oldSelections = GetRegionsWithIndicator();
            hasOldSelections = !oldSelections.empty();
        }
    }
    if (noTextSelected) {
        if (hasOldSelections) {
            auto restored = SetSelectionsFromStartEnds(oldSelections);
            selRanges = restored;
            noTextSelected = restored.size() == 1 && restored[0].second - restored[0].first == 0;
        }
    } else if (oneSelRange && firstSelLen == len) {
        noTextSelected = true; // selecting the whole doc reverts to no-selection mode
    }
    JNodePtr json;
    std::vector<JsonLint> lints;
    bool hasLints = true;
    std::vector<Comment> comments;
    bool hasComments = false;
    std::vector<bool> fatalErrors;
    std::string errorMessage;
    bool gotError = false;
    if (noTextSelected) {
        if (wasAutotriggered && len > sizeThreshold)
            return {ParserState::OK, nullptr, false, DocumentType::NONE};
        stopUsingSelections = true;
        std::string text = GetText();
        if (documentType == DocumentType::NONE || preferPreviousDocumentType) {
            DocumentType docTypeFromExtension = DocumentTypeFromFileExtension(FileExtension(fname));
            if (docTypeFromExtension != DocumentType::NONE) documentType = docTypeFromExtension;
            if (previouslyChosenDocType != DocumentType::NONE)
                documentType = previouslyChosenDocType;
            if (documentType == DocumentType::NONE) documentType = DocumentType::JSON;
        }
        if (documentType == DocumentType::INI) {
            try {
                json = sIniParser.Parse(text);
            } catch (const IniParserException& iniEx) {
                gotError = true;
                errorMessage = iniEx.what();
                lints.push_back(iniEx.ToJsonLint());
            } catch (const std::exception& ex) {
                gotError = true;
                errorMessage = ex.what();
            }
            fatalErrors.push_back(gotError);
            comments = sIniParser.comments;
            hasComments = true;
        } else if (documentType == DocumentType::REGEX) {
            json = JNode::Str(text);
            hasLints = false;
        } else {
            try {
                if (documentType == DocumentType::JSONL)
                    json = jsonParser.ParseJsonLines(text);
                else
                    json = jsonParser.Parse(text);
                lints = jsonParser.lint;
                fatalErrors.push_back(jsonParser.fatal());
                if (jsonParser.fatal()) {
                    gotError = true;
                    const JsonLint* fe = jsonParser.fatalError();
                    errorMessage = fe ? fe->message() : "fatal parse error";
                }
            } catch (const std::exception& ex) {
                lints = jsonParser.lint;
                gotError = true;
                errorMessage = HandleJsonParserError(text, fatalErrors, lints, jsonParser, ex);
            }
            if (jtSettings().remember_comments) {
                comments = jsonParser.comments;
                hasComments = true;
            }
        }
    } else {
        int64_t previouslySelectedIndicator = GetIndicatorCurrent();
        int64_t currentIndicator = gSelRememberIndicator1;
        ClearPreviouslyRememberedSelections(len, selRanges.size());
        auto obj = std::make_shared<JObject>();
        json = obj;
        int64_t combinedLength = 0;
        for (auto& [start, end] : selRanges) combinedLength += end - start;
        if (wasAutotriggered && combinedLength > sizeThreshold)
            return {ParserState::OK, nullptr, false, DocumentType::NONE};
        stopUsingSelections = false;
        for (auto& [start, end] : selRanges) {
            std::string selRange = GetSlice(start, end);
            JNodePtr subJson;
            if (documentType == DocumentType::REGEX) {
                subJson = JNode::Str(selRange, 0);
            } else {
                try {
                    subJson = jsonParser.Parse(selRange);
                    for (const JsonLint& l : jsonParser.lint) lints.push_back(l);
                    fatalErrors.push_back(jsonParser.fatal());
                    if (jsonParser.fatal() && !gotError) {
                        gotError = true;
                        const JsonLint* fe = jsonParser.fatalError();
                        errorMessage = fe ? fe->message() : "fatal parse error";
                    }
                } catch (const std::exception& ex) {
                    subJson = JNode::Null();
                    gotError = true;
                    errorMessage = HandleJsonParserError(selRange, fatalErrors, lints, jsonParser, ex);
                }
            }
            currentIndicator = ApplyAndSwapIndicator(currentIndicator, start, end - start);
            obj->children.set(std::to_string(start) + "," + std::to_string(end), subJson);
        }
        SetIndicatorCurrent(previouslySelectedIndicator);
    }
    if (!isRecursion && oneSelRange && NoSelectionIsValidJson(json, !noTextSelected, fatalErrors)) {
        SetSelectionsFromStartEnds(oldSelections);
        return TryParseJson(documentType, wasAutotriggered, preferPreviousDocumentType, true);
    }
    size_t lintCount = hasLints ? lints.size() : 0;
    JsonFileInfo& info = AddJsonForFile(fname, json);
    bool fatal = gotError;
    info.documentType = fatal ? DocumentType::NONE : documentType;
    if (stopUsingSelections)
        info.usesSelections = false;
    else
        info.usesSelections = info.usesSelections || !noTextSelected;
    info.lints = lints;
    info.hasLints = hasLints;
    ParserState parserStateToSet = fatal ? ParserState::FATAL : jsonParser.state;
    SetStatusBarSectionText(parserStateToSet, fname, info, documentType, lintCount);
    if (lintCount > 0 && jtSettings().offer_to_show_lint && !wasAutotriggered) {
        std::string msg = "There were " + std::to_string(lintCount) +
            " syntax errors in the document. Would you like to see them?\n"
            "(You can turn off these prompts in the settings (offer_to_show_lint setting))";
        if (MsgBoxYesNo("View syntax errors in document?", msg)) {
            if (jtErrorFormIsVisible()) jtErrorFormHide();
            OpenErrorForm(activeFname, true);
        }
    }
    if (lintCount == 0 && jtErrorFormIsVisible())
        jtErrorFormReload(activeFname, {});
    if (fatal) {
        if (lintCount > 0 && noTextSelected && !wasAutotriggered)
            GoToLegalPos(lints[lintCount - 1].pos);
        if (!errorFormTriggeredParse)
            MsgBoxError(std::string("Error while trying to parse ") +
                            DocumentTypeSuperTypeName(documentType),
                        "Could not parse the document because of error\n" + errorMessage);
    }
    if (info.tv) {
        [info.tv setJson:json];
        info.tv.documentTypeIndexChangeWasAutomatic = true;
        [info.tv setDocumentTypeComboBoxIndex:documentType];
        info.tv.documentTypeIndexChangeWasAutomatic = false;
    }
    info.comments = comments;
    info.hasComments = hasComments;
    return {parserStateToSet, json, info.usesSelections, documentType};
}

// ── reformatting ─────────────────────────────────────────────────────────────
void RenameAll(const std::map<std::string, std::pair<std::string, JNodePtr>>& keyChanges,
               JObject* obj) {
    for (auto& [oldKey, change] : keyChanges) obj->children.erase(oldKey);
    for (auto& [oldKey, change] : keyChanges) {
        if (!change.first.empty()) obj->children.set(change.first, change.second);
    }
}

std::map<std::string, std::pair<std::string, JNodePtr>>
ReformatFileWithJson(const JNodePtr& json,
                     const std::function<std::string(const JNodePtr&)>& formatter,
                     bool usesSelections) {
    std::map<std::string, std::pair<std::string, JNodePtr>> keyChanges;
    int64_t len = DocLength();
    BeginUndoAction();
    JsonFileInfo* infoP = nullptr;
    if (usesSelections) {
        JObject* obj = AsObj(json);
        int64_t delta = 0;
        int64_t previouslySelectedIndicator = GetIndicatorCurrent();
        // sort selection keys by start position
        std::vector<std::pair<std::string, JNodePtr>> keyvalues(obj->children.begin(),
                                                                obj->children.end());
        ClearPreviouslyRememberedSelections(len, keyvalues.size());
        int64_t currentIndicator = gSelRememberIndicator1;
        std::sort(keyvalues.begin(), keyvalues.end(), [](const auto& a, const auto& b) {
            return StartEndCompareByStart(a.first, b.first) < 0;
        });
        for (auto& [key, value] : keyvalues) {
            int64_t start, end;
            if (!ParseStartEnd(key, start, end)) continue;
            int64_t oldLen = end - start;
            std::string printed;
            try {
                printed = formatter(value);
            } catch (const std::exception& ex) {
                MsgBoxError("Error while reformatting JSON", ex.what());
                continue;
            }
            int64_t newStart = start + delta;
            int64_t newCount = (int64_t)printed.size();
            DeleteRange(newStart, oldLen);
            InsertText(newStart, printed);
            currentIndicator = ApplyAndSwapIndicator(currentIndicator, newStart, newCount);
            int64_t newEnd = newStart + newCount;
            delta = newEnd - end;
            keyChanges[key] = {std::to_string(newStart) + "," + std::to_string(newEnd), value};
        }
        RenameAll(keyChanges, obj);
        std::vector<std::string> newKeys;
        for (auto& kv : obj->children) newKeys.push_back(kv.first);
        SetSelectionsFromStartEnds(newKeys);
        SetIndicatorCurrent(previouslySelectedIndicator);
    } else if (TryGetInfoForFile(activeFname, &infoP) && infoP->documentType == DocumentType::INI &&
               AsObj(json)) {
        std::string iniText;
        try {
            iniText = AsObj(json)->ToIniFile(infoP->comments);
        } catch (const std::exception& ex) {
            MsgBoxError("Error while reformatting INI file",
                        std::string("Error while reformatting INI file:\n") + ex.what());
            EndUndoAction();
            return keyChanges;
        }
        SetText(iniText);
    } else {
        try {
            SetText(formatter(json));
        } catch (const std::exception& ex) {
            MsgBoxError("Programmatic error while reformatting JSON",
                        std::string("While attempting to reformat the file's JSON, got a "
                                    "programmatic error:\n") +
                            RemesParser::PrettifyException(ex));
        }
    }
    JsonFileInfo& info = AddJsonForFile(activeFname, json);
    RemoveTrailingSOH();
    if (!usesSelections) ScrollCaret();
    EndUndoAction();
    sLastEditedTime = DBL_MAX; // avoid redundant parsing
    if (!usesSelections) SetLangBasedOnDocType(false, false, info.documentType);
    return keyChanges;
}

void PrettyPrintJsonInNewFile(const JNodePtr& json) {
    FileNew();
    ReformatFileWithJson(json, PrettyPrintFromSettings, false);
}

// ── path to position ─────────────────────────────────────────────────────────
std::string PathToPosition(KeyStyle style, char separator, int64_t pos) {
    if (pos == -1) pos = GetCurrentPos();
    std::string fname = GetCurrentFilePath();
    JNodePtr json;
    bool usesSelections = false;
    JsonFileInfo* info = nullptr;
    if (TryGetInfoForFile(fname, &info)) {
        json = info->json;
        usesSelections = info->usesSelections;
    } else {
        JTGrepperForm* grepperForm = [JTGrepperForm shared];
        if (grepperForm && [grepperForm tv] && [[grepperForm tv] fname] == fname)
            json = [grepperForm grepper]->fnameJsons;
    }
    if (!json) {
        ParseResult pr = TryParseJson(DocumentType::JSON, false, true);
        if (pr.parserState == ParserState::FATAL || !pr.node) return "";
        json = pr.node;
        usesSelections = pr.usesSelections;
    }
    try {
        if (usesSelections) {
            auto [start, end] = GetEnclosingRememberedSelection(pos);
            if (start < 0) return "";
            std::string selText = GetSlice(start, end);
            JsonParser parser = JsonParserFromSettings();
            JNodePtr selJson;
            try {
                selJson = parser.Parse(selText);
            } catch (...) {
                return "";
            }
            if (parser.fatal()) return "";
            std::string formattedKey = JNode::FormatKey(
                std::to_string(start) + "," + std::to_string(end), style, separator);
            return formattedKey + selJson->PathToPosition(pos - start, style, separator);
        }
        return json->PathToPosition(pos, style, separator);
    } catch (const std::exception& ex) {
        MsgBoxError("Error while formatting path to current position", ex.what());
        return "";
    }
}

void SetPathSeparatorFromSettings() {
    try {
        JsonParser parser;
        JNodePtr parsed = parser.ParseString(jtSettings().path_separator);
        std::string sepStr = parsed->GetStr();
        // one character OR one codepoint ≤ 0x7F (the engine separator is a byte)
        if (sepStr.size() != 1)
            throw JsonToolsError(
                "path_separator setting must be a JSON string containing exactly one character");
        JNode::ThrowIfPathSeparatorInvalid(sepStr[0]);
        pathSeparator = sepStr[0];
    } catch (const std::exception& ex) {
        MsgBoxError("Could not change path_separator setting",
                    std::string("path_separator setting ") + jtSettings().path_separator +
                        " is invalid:\n" + RemesParser::PrettifyException(ex));
        jtSettings().path_separator = "\"\\u0001\"";
        pathSeparator = JNode::DEFAULT_PATH_SEPARATOR;
        jtSettingsSave();
    }
}

int64_t EndOfJNodeAtPos(int64_t startUtf8Pos, int64_t end) {
    std::string slice = GetSlice(startUtf8Pos, end);
    JsonParser parser(LoggerLevel::JSON5);
    try {
        parser.ParseSomething(slice, 0);
    } catch (...) {
        return startUtf8Pos;
    }
    return startUtf8Pos + parser.ii;
}

void SelectAllChildren(std::vector<int64_t> positions, bool isJsonLines) {
    int64_t len = DocLength();
    std::sort(positions.begin(), positions.end());
    positions.erase(std::unique(positions.begin(), positions.end()), positions.end());
    if (positions.empty()) return;
    int64_t minPos = positions[0];
    if (!isJsonLines && minPos == 0) {
        MsgBoxWarning("Can't select all children",
                      "Cannot select all children because one or more of the children does not "
                      "correspond to a JSON node in the document");
        return;
    }
    std::string slice = GetSlice(minPos, len);
    JsonParser parser(LoggerLevel::JSON5);
    std::vector<std::string> selections;
    for (int64_t startPos : positions) {
        int64_t offset = startPos - minPos;
        if (offset < 0 || offset >= (int64_t)slice.size()) continue;
        parser.Reset();
        parser.ii = offset;
        try {
            parser.ParseSomething(slice, 0);
            selections.push_back(std::to_string(startPos) + "," +
                                 std::to_string(minPos + parser.ii));
        } catch (...) {
        }
    }
    SetSelectionsFromStartEnds(selections);
}

// ── schema cache + schemasToFnamePatterns ────────────────────────────────────
class SchemaCache {
public:
    explicit SchemaCache(size_t capacity) : cache_(capacity) {}

    bool Get(const std::string& fname, JsonSchemaValidator::ValidationFunc& validator) {
        auto lastIt = lastRetrieved_.find(fname);
        JsonSchemaValidator::ValidationFunc cached;
        if (lastIt == lastRetrieved_.end() || !cache_.TryGetValue(fname, cached))
            return false;
        struct stat st;
        if (stat(fname.c_str(), &st) == 0 && st.st_mtime > lastIt->second)
            return TryAdd(fname, validator);
        validator = cached;
        return true;
    }

    bool TryAdd(const std::string& fname, JsonSchemaValidator::ValidationFunc& validator) {
        std::ifstream f(fname, std::ios::binary);
        if (!f) {
            MsgBoxError("Error while trying to parse schema",
                        "Could not read the schema file at path " + fname);
            return false;
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        JNodePtr schema;
        try {
            JsonParser parser(LoggerLevel::JSON5);
            schema = parser.Parse(ss.str());
        } catch (const std::exception& ex) {
            MsgBoxError("Error while trying to parse schema",
                        "While trying to parse the schema at path " + fname +
                            ", the following error occurred:\n" +
                            RemesParser::PrettifyException(ex));
            return false;
        }
        if (!schema) return false;
        if (cache_.IsFull() && cache_.Count() > 0) lastRetrieved_.erase(cache_.OldestKey());
        try {
            validator = JsonSchemaValidator::CompileValidationFunc(
                schema, jtSettings().max_schema_validation_problems);
            lastRetrieved_[fname] = time(nullptr);
            cache_.Set(fname, validator);
            return true;
        } catch (const std::exception& ex) {
            MsgBoxError("Error while compiling JSON schema",
                        "While compiling schema for file \"" + fname + "\", got exception:\n" +
                            RemesParser::PrettifyException(ex));
            return false;
        }
    }

    void Invalidate() {
        for (auto& kv : lastRetrieved_) kv.second = 0;
    }

private:
    LruCache<std::string, JsonSchemaValidator::ValidationFunc> cache_;
    std::map<std::string, time_t> lastRetrieved_;
};

static SchemaCache& schemaCache() {
    static SchemaCache c(16);
    return c;
}

static JsonSchemaValidator::ValidationFunc& schemasToFnamePatternsSchema() {
    static JsonSchemaValidator::ValidationFunc fn = [] {
        JsonParser parser;
        JNodePtr schema = parser.Parse(
            "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\","
            "\"properties\":{},\"required\":[],\"type\":\"object\","
            "\"patternProperties\":{"
            "\".+\":{\"items\":{\"type\":\"string\"},\"minItems\":1,\"type\":\"array\"},"
            "\"^$\":false"
            "}}");
        return JsonSchemaValidator::CompileValidationFunc(schema, 0);
    }();
    return fn;
}

static void SetSchemasToFnamePatternsFname() {
    sSchemasToFnamePatternsFname = ConfigSubDirectory() + "/schemasToFnamePatterns.json";
}

static void WriteSchemasToFnamePatternsFile(const std::shared_ptr<JObject>& schemasToPatterns) {
    std::ofstream fp(sSchemasToFnamePatternsFname, std::ios::trunc);
    if (!fp) return;
    fp << "// this file determines when automatic JSON validation should be performed\n"
       << "// each key must be the filename of a JSON schema file\n"
       << "// each value must be a non-empty list of valid regular expressions "
          "(e.g., [\"blah.*\\\\.txt\"])\n"
       << "// thus, if this file contained {\"/path/to/foo_schema.json\": [\"blah.*\\\\.txt\"]}\n"
       << "// it would automatically validate with \"/path/to/foo_schema.json\" whenever a "
          "\".txt\" file with the substring \"blah\" in its name was opened.\n";
    // print without the compiled-JRegex children
    auto cleanObj = std::make_shared<JObject>();
    for (auto& [fname, patterns] : schemasToPatterns->children) {
        if (JArray* arr = AsArr(patterns)) {
            auto cleanArr = std::make_shared<JArray>();
            for (JNodePtr& pat : arr->children) {
                if (auto* jregex = dynamic_cast<JRegex*>(pat.get()))
                    cleanArr->children.push_back(JNode::Str(jregex->regex->Pattern()));
                else
                    cleanArr->children.push_back(pat);
            }
            cleanObj->children.set(fname, cleanArr);
        }
    }
    fp << cleanObj->PrettyPrint();
}

static void ParseSchemasToFnamePatternsFile() {
    JtSettings& settings = jtSettings();
    bool wasAutoValidating = settings.auto_validate;
    settings.auto_validate = false;
    struct stat st;
    if (stat(sSchemasToFnamePatternsFname.c_str(), &st) != 0 || st.st_size == 0)
        WriteSchemasToFnamePatternsFile(sSchemasToFnamePatterns);
    std::ifstream fp(sSchemasToFnamePatternsFname, std::ios::binary);
    if (!fp) {
        settings.auto_validate = wasAutoValidating;
        return;
    }
    std::ostringstream ss;
    ss << fp.rdbuf();
    try {
        JsonParser jParser(LoggerLevel::JSON5);
        JNodePtr parsed = jParser.Parse(ss.str());
        auto asObj = std::dynamic_pointer_cast<JObject>(parsed);
        if (!asObj) throw JsonToolsError("schemasToFnamePatterns.json must be an object");
        sSchemasToFnamePatterns = asObj;
    } catch (const std::exception& ex) {
        MsgBoxError("Couldn't parse schemasToFnamePatterns.json",
                    std::string("Failed to parse schemasToFnamePatterns.json. Got error\n") +
                        ex.what());
        settings.auto_validate = wasAutoValidating;
        return;
    }
    std::vector<JsonLint> lints;
    bool validates = schemasToFnamePatternsSchema()(sSchemasToFnamePatterns, lints);
    if (!validates && !lints.empty()) {
        MsgBoxError("schemasToFnamePatterns.json badly formatted",
                    "schemasToFnamePatterns.json must be an object mapping filenames to "
                    "non-empty arrays of valid regexes (strings).\nThere were the following "
                    "validation problem(s):\n" +
                        JsonSchemaValidator::LintsAsJArrayString(lints));
        sSchemasToFnamePatterns = std::make_shared<JObject>();
        settings.auto_validate = wasAutoValidating;
        return;
    }
    std::vector<std::string> fnames;
    for (auto& kv : sSchemasToFnamePatterns->children) fnames.push_back(kv.first);
    for (const std::string& fname : fnames) {
        struct stat fst;
        if (stat(fname.c_str(), &fst) != 0) {
            MsgBoxError("No schema exists at path " + fname, "No schema exists at path " + fname);
            sSchemasToFnamePatterns->children.erase(fname);
            continue;
        }
        JsonSchemaValidator::ValidationFunc unused;
        if (!schemaCache().TryAdd(fname, unused)) {
            sSchemasToFnamePatterns->children.erase(fname);
            continue;
        }
        JNodePtr patternsNode;
        if (!sSchemasToFnamePatterns->TryGetValue(fname, patternsNode) || !AsArr(patternsNode))
            continue;
        auto regexes = std::make_shared<JArray>();
        for (JNodePtr& patternNode : AsArr(patternsNode)->children) {
            std::string pattern = patternNode->GetStr();
            try {
                regexes->children.push_back(
                    std::make_shared<JRegex>(std::make_shared<Regex>(pattern)));
            } catch (const std::exception& ex) {
                MsgBoxError("Regex did not compile (in schemasToFnamePatterns.json)",
                            "While testing all the regexes associated with file " + fname +
                                ",\nregular expression " + pattern +
                                " failed to compile due to an error:\n" + ex.what());
            }
        }
        if (regexes->children.empty())
            sSchemasToFnamePatterns->children.erase(fname);
        else
            sSchemasToFnamePatterns->children.set(fname, regexes);
    }
    settings.auto_validate = wasAutoValidating;
}

void ValidateJson(std::string schemaPath, bool messageOnSuccess, bool ignoreSelections,
                  bool wasAutotriggered) {
    ParseResult pr = TryParseJson(DocumentType::JSON, wasAutotriggered, true, false,
                                  ignoreSelections);
    std::string curFname = GetCurrentFilePath();
    if (pr.parserState == ParserState::FATAL || !pr.node) {
        RefreshErrorFormIfShown(curFname, wasAutotriggered);
        return;
    }
    if (schemaPath.empty()) {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.title = @"Select JSON schema file to validate against";
        panel.allowsMultipleSelection = NO;
        panel.canChooseDirectories = NO;
        if ([panel runModal] != NSModalResponseOK || !panel.URL) return;
        schemaPath = panel.URL.path.UTF8String;
    }
    JsonSchemaValidator::ValidationFunc validator;
    if (!schemaCache().Get(schemaPath, validator) && !schemaCache().TryAdd(schemaPath, validator))
        return;
    std::vector<JsonLint> problems;
    bool validates;
    try {
        validates = validator(pr.node, problems);
    } catch (const std::exception& e) {
        MsgBoxError("Error while validating JSON against schema",
                    "While validating JSON against the schema at path " + schemaPath +
                        ", the following error occurred:\n" + RemesParser::PrettifyException(e));
        return;
    }
    JsonFileInfo* info = nullptr;
    if (!TryGetInfoForFile(curFname, &info)) info = &AddJsonForFile(curFname, pr.node);
    info->filenameOfMostRecentValidatingSchema = schemaPath;
    for (const JsonLint& p : problems) info->lints.push_back(p);
    info->hasLints = true;
    size_t lintCount = info->lints.size();
    RefreshErrorFormIfShown(curFname, wasAutotriggered);
    SetStatusBarSectionText(pr.parserState, curFname, *info, pr.documentType, lintCount);
    if (!validates && !problems.empty()) {
        const JsonLint& firstProblem = problems[0];
        if (!wasAutotriggered) GoToLegalPos(firstProblem.pos);
        MsgBoxWarning("Validation failed...",
                      "The JSON in file " + curFname +
                          " DOES NOT validate against the schema at path " + schemaPath +
                          ". Problem 1 of " + std::to_string(problems.size()) + ":\n" +
                          firstProblem.ToString());
        return;
    }
    if (messageOnSuccess)
        MsgBoxInfo("Validation succeeded!", "The JSON in file " + curFname +
                                                " validates against the schema at path " +
                                                schemaPath + ".");
}

bool ValidateIfFilenameMatches(const std::string& fname, bool wasAutotriggered) {
    if (wasAutotriggered && DocLength() > jtSettings().max_file_size_MB_slow_actions * 1e6)
        return false;
    for (auto& [schemaFname, patternsNode] : sSchemasToFnamePatterns->children) {
        JArray* fnamePatterns = AsArr(patternsNode);
        if (!fnamePatterns) continue;
        for (JNodePtr& pat : fnamePatterns->children) {
            auto* jregex = dynamic_cast<JRegex*>(pat.get());
            if (!jregex || !jregex->regex->IsMatch(fname)) continue;
            ValidateJson(schemaFname, false, true, wasAutotriggered);
            return true;
        }
    }
    return false;
}

// ── error form ───────────────────────────────────────────────────────────────
void OpenErrorForm(const std::string& fname, bool wasAutoTriggered) {
    bool wasVisible = jtErrorFormIsVisible();
    JsonFileInfo* info = nullptr;
    bool haveInfo = TryGetInfoForFile(fname, &info);
    if ((!haveInfo || !info->hasLints || info->lints.empty()) && !wasAutoTriggered) {
        MsgBoxInfo("No JSON syntax errors for this file",
                   "No JSON syntax errors (at or below the current logger_level) for " + fname);
        return;
    }
    if (wasVisible) {
        jtErrorFormHide();
        return;
    }
    jtErrorFormShow(fname, haveInfo ? info->lints : std::vector<JsonLint>());
}

void RefreshErrorFormIfShown(const std::string& fname, bool suppressIfFormNotVisible) {
    if (suppressIfFormNotVisible && !jtErrorFormIsVisible()) return;
    JsonFileInfo* info = nullptr;
    if (!TryGetInfoForFile(fname, &info) || !info->hasLints) return;
    if (jtErrorFormIsVisible()) jtErrorFormReload(fname, info->lints);
}

// ── menu commands ────────────────────────────────────────────────────────────
void CmdDocs() { OpenUrlInWebBrowser(PLUGIN_REPO_URL); }

void CmdPrettyPrintJson() {
    ParseResult pr = TryParseJson(DocumentType::JSON, false, true);
    JsonFileInfo* info = nullptr;
    if (pr.parserState == ParserState::FATAL || !pr.node || !TryGetInfoForFile(activeFname, &info))
        return;
    const JtSettings& s = jtSettings();
    bool isJsonLines = info->documentType == DocumentType::JSONL;
    if (isJsonLines) {
        if (s.ask_before_pretty_printing_json_lines == AskUserWhetherToDoThing::DONT_DO_DONT_ASK)
            return;
        if (s.ask_before_pretty_printing_json_lines == AskUserWhetherToDoThing::ASK_BEFORE_DOING &&
            !MsgBoxYesNo("Pretty-print JSON Lines document?",
                         "Pretty-printing a JSON Lines document will generally lead to it no "
                         "longer being a valid JSON Lines document. Pretty-print anyway?"))
            return;
    }
    int indent = s.tab_indent_pretty_print ? 1 : s.indent_pretty_print;
    std::function<std::string(const JNodePtr&)> formatter;
    if (UseComments(*info)) {
        std::vector<Comment>* comments = &info->comments;
        formatter = [indent, comments, &s](const JNodePtr& x) {
            return x->PrettyPrintWithCommentsAndChangePositions(
                *comments, indent, s.sort_keys, s.tab_indent_pretty_print ? '\t' : ' ',
                s.pretty_print_style);
        };
    } else {
        formatter = PrettyPrintFromSettings;
    }
    if (isJsonLines) info->documentType = DocumentType::JSON;
    ReformatFileWithJson(pr.node, formatter, pr.usesSelections);
}

void CmdCompressJson() {
    ParseResult pr = TryParseJson(DocumentType::JSON, false, true);
    JsonFileInfo* info = nullptr;
    if (pr.parserState == ParserState::FATAL || !pr.node || !TryGetInfoForFile(activeFname, &info))
        return;
    const JtSettings& s = jtSettings();
    std::function<std::string(const JNodePtr&)> formatter;
    if (UseComments(*info)) {
        std::vector<Comment>* comments = &info->comments;
        formatter = [comments, &s](const JNodePtr& x) {
            return x->ToStringWithCommentsAndChangePositions(*comments, s.sort_keys);
        };
    } else if (s.minimal_whitespace_compression) {
        formatter = [&s](const JNodePtr& x) {
            return x->ToStringAndChangePositions(s.sort_keys, ":", ",");
        };
    } else {
        formatter = [&s](const JNodePtr& x) { return x->ToStringAndChangePositions(s.sort_keys); };
    }
    ReformatFileWithJson(pr.node, formatter, pr.usesSelections);
}

void CmdCheckJsonSyntaxNow() {
    std::string curFname = GetCurrentFilePath();
    TryParseJson(FileExtension(curFname) == "jsonl" ? DocumentType::JSONL : DocumentType::JSON);
    if (jtErrorFormIsVisible() && !jtSettings().offer_to_show_lint)
        RefreshErrorFormIfShown(curFname, false);
}

void CmdCopyPathToCurrentPosition() {
    int64_t pos = GetCurrentPos();
    std::string result = PathToPosition(jtSettings().key_style, pathSeparator, pos);
    if (result.empty()) {
        MsgBoxError("Could not find a node at this position",
                    "Did not find a node at position " + std::to_string(pos) + " of this file");
        return;
    }
    TryCopyToClipboard(result);
}

void CmdSelectEveryValidJson() {
    int64_t utf8Len = DocLength();
    auto selections = GetSelectedRanges();
    if (NoTextSelected(selections)) {
        selections.clear();
        selections.emplace_back(0, utf8Len);
    }
    std::sort(selections.begin(), selections.end());
    JsonParser jsonChecker(LoggerLevel::NAN_INF);
    std::vector<std::string> startEnds;
    const std::string& startChars = jtSettings().try_parse_start_chars;
    for (auto& [start, end] : selections) {
        std::string text = GetSlice(start, end);
        int64_t ii = 0;
        int64_t len = (int64_t)text.size();
        while (ii < len) {
            char c = text[(size_t)ii];
            if (startChars.find(c) != std::string::npos) {
                int64_t startPos = start + ii;
                jsonChecker.Reset();
                jsonChecker.ii = ii;
                try {
                    jsonChecker.ParseSomething(text, 0);
                } catch (...) {
                }
                ii = jsonChecker.ii;
                if (!jsonChecker.exitedEarly()) {
                    int64_t endPos = start + ii;
                    if (endPos > utf8Len) endPos = utf8Len;
                    startEnds.push_back(std::to_string(startPos) + "," + std::to_string(endPos));
                }
            } else {
                ii++;
            }
        }
    }
    if (startEnds.empty()) {
        MsgBoxWarning("No valid JSON elements found",
                      "No valid JSON elements starting with chars " + startChars +
                          " were found in the document");
    } else {
        SetSelectionsFromStartEnds(startEnds);
    }
    TryParseJson(DocumentType::JSON, false);
}

void CmdOpenJsonTree(DocumentType documentType) {
    if (openTreeViewer && ![openTreeViewer view]) openTreeViewer = nil;
    JsonFileInfo* info = nullptr;
    bool haveInfo = TryGetInfoForFile(activeFname, &info);
    JTGrepperForm* grepperForm = [JTGrepperForm shared];
    if (haveInfo && info->tv) {
        bool wasVisible = [info->tv isVisibleOnScreen];
        if (!wasVisible && grepperForm && [grepperForm tv] &&
            GetCurrentFilePath() == [[grepperForm tv] fname]) {
            // toggling while looking at the grepper buffer toggles the grepper tree
            if ([[grepperForm tv] isVisibleOnScreen])
                [[grepperForm tv] hidePanel];
            else
                [[grepperForm tv] showPanel];
            return;
        }
        [info->tv closePanel];
        JTTreeViewer* closing = info->tv;
        info->tv = nil;
        if (openTreeViewer == closing) openTreeViewer = nil;
        if (wasVisible) return;
    }
    ParseResult pr = TryParseJson(documentType);
    if (!pr.node || !TryGetInfoForFile(activeFname, &info)) return;
    openTreeViewer = [[JTTreeViewer alloc] initWithJson:pr.node];
    info->tv = openTreeViewer;
    DisplayJsonTree(openTreeViewer, pr.node, activeFname, pr.usesSelections, pr.documentType,
                    pr.parserState == ParserState::FATAL);
}

void DisplayJsonTree(JTTreeViewer* treeViewer, const JNodePtr& json, const std::string& title,
                     bool usesSelections, DocumentType documentType, bool fatalParserError) {
    SetLangBasedOnDocType(fatalParserError, usesSelections, documentType);
    [treeViewer renameTo:title isFilename:!treeViewer.isGrepperTree];
    [treeViewer jsonTreePopulate:json];
    [treeViewer showPanel];
    [treeViewer focusQueryBox];
}

void CmdOpenGrepperForm() { [JTGrepperForm showOrFocus]; }

void CmdOpenSortForm() {
    jtShowSortForm(PathToPosition(KeyStyle::RemesPath, JNode::DEFAULT_PATH_SEPARATOR));
}

void CmdOpenSettings() {
    JtSettings& s = jtSettings();
    bool oldUseNppStyling = s.use_npp_styling;
    float oldTreeViewFontSize = s.tree_view_font_size;
    std::string oldPathSepStr = s.path_separator;
    int oldMaxSchemaValidationProblems = s.max_schema_validation_problems;
    if (!jtRunSettingsDialog()) return;
    if (s.path_separator != oldPathSepStr) SetPathSeparatorFromSettings();
    if (s.tree_view_font_size != oldTreeViewFontSize || s.use_npp_styling != oldUseNppStyling)
        OnThemeChanged();
    if (s.max_schema_validation_problems != oldMaxSchemaValidationProblems)
        schemaCache().Invalidate();
}

void CmdValidateJson() { ValidateJson("", true, false, false); }

void CmdMapSchemasToFnamePatterns() {
    struct stat st;
    if (stat(sSchemasToFnamePatternsFname.c_str(), &st) != 0)
        WriteSchemasToFnamePatternsFile(sSchemasToFnamePatterns);
    OpenFile(sSchemasToFnamePatternsFname);
}

void CmdGenerateJsonSchema() {
    ParseResult pr = TryParseJson(DocumentType::JSON, false, true);
    if (pr.parserState == ParserState::FATAL || !pr.node) return;
    JNodePtr schema;
    try {
        schema = JsonSchemaMaker::GetSchema(pr.node);
    } catch (const std::exception& ex) {
        MsgBoxError("JSON schema generation error",
                    std::string("Could not generate a JSON schema. Got the following error:\n") +
                        ex.what());
        return;
    }
    PrettyPrintJsonInNewFile(schema);
}

void CmdGenerateRandomJson() {
    ParseResult pr = TryParseJson(DocumentType::JSON, false, true);
    if (pr.parserState == ParserState::FATAL || !pr.node) return;
    const JtSettings& s = jtSettings();
    JNodePtr randomJson;
    try {
        // try to use the currently open file as a schema
        randomJson = RandomJsonFromSchema::RandomJson(pr.node, s.minArrayLength, s.maxArrayLength,
                                                      s.extended_ascii_strings,
                                                      s.generate_random_patterns);
    } catch (...) {
        try {
            // the file wasn't a schema; build a schema from it and use that
            JNodePtr schema = JsonSchemaMaker::GetSchema(pr.node);
            randomJson = RandomJsonFromSchema::RandomJson(schema, s.minArrayLength,
                                                          s.maxArrayLength,
                                                          s.extended_ascii_strings,
                                                          s.generate_random_patterns);
        } catch (const std::exception& ex) {
            MsgBoxError("Error while generating random JSON from schema",
                        std::string("While trying to generate random JSON from this schema, got "
                                    "an error:\n") +
                            ex.what());
            return;
        }
    }
    PrettyPrintJsonInNewFile(randomJson);
}

void CmdRunTests() {
    // PORT NOTE: the upstream in-app test runner is replaced by the repo's
    // headless harness (test/engine_test.cpp), which runs the same suites.
    MsgBoxInfo("JsonTools tests",
               "On macOS the JsonTools engine tests run as a separate program rather than inside "
               "Nextpad++.\nThey are run on every build of the plugin (2249 checks).");
}

void CmdShowAboutForm() { jtShowAboutDialog(); }

void CmdOpenErrorForm() { OpenErrorForm(activeFname, false); }

void CmdDumpYaml() {
    if (!MsgBoxOkCancel("JSON to YAML feature has some bugs",
                        "This feature has known bugs that may result in invalid YAML being "
                        "emitted. Use it anyway?"))
        return;
    ParseResult pr = TryParseJson(DocumentType::JSON, false, true);
    if (pr.parserState == ParserState::FATAL || !pr.node) return;
    std::string yaml;
    try {
        yaml = sYamlDumper.Dump(pr.node);
    } catch (const std::exception& ex) {
        MsgBoxError("Error while trying to convert JSON to YAML",
                    std::string("Could not convert the JSON to YAML because of error\n") +
                        RemesParser::PrettifyException(ex));
        return;
    }
    FileNew();
    SetText(yaml);
    ScrollCaret();
    RemoveTrailingSOH();
    SetCurrentLang(L_YAML);
}

void CmdOpenJsonTreeJsonLines() { CmdOpenJsonTree(DocumentType::JSONL); }

void CmdDumpJsonLines() {
    ParseResult pr = TryParseJson(DocumentType::JSON, false, true);
    if (pr.parserState == ParserState::FATAL || !pr.node) return;
    if (!AsArr(pr.node)) {
        MsgBoxError("Only arrays can be converted to JSON Lines",
                    "Only JSON arrays can be converted to JSON Lines format.");
        return;
    }
    FileNew();
    ReformatFileWithJson(pr.node, ToJsonLinesFromSettings, pr.usesSelections);
}

void CmdDumpSelectedTextAsJsonString() {
    auto selections = GetSelectedRanges();
    if (NoTextSelected(selections)) {
        std::string text = GetText();
        PrettyPrintJsonInNewFile(JNode::Str(text));
    } else {
        std::string sb;
        std::sort(selections.begin(), selections.end());
        for (auto& [start, end] : selections) {
            sb += JNode::Str(GetSlice(start, end))->ToString();
            sb += "\r\n";
        }
        FileNew();
        SetText(sb);
        SetCurrentLang(L_JSON);
        RemoveTrailingSOH();
    }
}

static bool TryGetSelectedJsonStringValue(JsonParser& jsonParser, int64_t start, int64_t end,
                                          std::string& out) {
    std::string text = (start < 0 || end < 0) ? GetText() : GetSlice(start, end);
    try {
        JNodePtr textNode = jsonParser.Parse(text);
        if (textNode->IsStrValue()) {
            out = textNode->GetStr();
            return true;
        }
    } catch (...) {
    }
    MsgBoxError("Failed to parse selected text as JSON", "Selected text is not a JSON string");
    return false;
}

void CmdDumpSelectedJsonStringsAsText() {
    auto selections = GetSelectedRanges();
    std::sort(selections.begin(), selections.end());
    std::string sb;
    JsonParser jsonParser = JsonParserFromSettings();
    if (NoTextSelected(selections)) {
        std::string value;
        if (!TryGetSelectedJsonStringValue(jsonParser, -1, -1, value)) return;
        sb += value;
    } else {
        for (auto& [start, end] : selections) {
            std::string value;
            if (!TryGetSelectedJsonStringValue(jsonParser, start, end, value)) return;
            sb += value;
            sb += "\r\n";
        }
    }
    FileNew();
    SetText(sb);
}

void CmdOpenJsonTreeIni() { CmdOpenJsonTree(DocumentType::INI); }

void CmdRegexSearchToJson() { jtShowRegexSearchForm(); }

// ── notifications ────────────────────────────────────────────────────────────
void OnReady() {
    SetSchemasToFnamePatternsFname();
    struct stat st;
    if (stat(sSchemasToFnamePatternsFname.c_str(), &st) != 0 || st.st_size == 0)
        WriteSchemasToFnamePatternsFile(sSchemasToFnamePatterns);
    ParseSchemasToFnamePatternsFile();
    AllocateSelectionIndicators();
    HideSelectionRememberingIndicators();
    SetPathSeparatorFromSettings();
}

void OnFileBeforeOpen() { sBufferFinishedOpening = false; }

void OnBufferActivated(uintptr_t bufferId) {
    sBufferFinishedOpening = true;
    RefreshCurrentScintilla();
    std::string newFname = GetFilePathFromBufferId(bufferId);
    if (!newFname.empty()) sBufferIdToFname[bufferId] = newFname;
    JsonFileInfo* newInfo = nullptr;
    bool newFileHasInfo = TryGetInfoForFile(newFname, &newInfo);
    if (newFileHasInfo) {
        if (newInfo->usesSelections && newInfo->json) {
            if (JObject* newObj = AsObj(newInfo->json); newObj && newObj->Length() > 0) {
                // keep a zero-length cursor at the first JSON selection so the host
                // doesn't clobber the remembered selections
                int64_t firstSelStart = INT64_MAX;
                for (auto& kv : newObj->children) {
                    int64_t s = StartFromStartEnd(kv.first);
                    if (s < firstSelStart) firstSelStart = s;
                }
                if (firstSelStart != INT64_MAX) {
                    std::string se = std::to_string(firstSelStart) + "," +
                                     std::to_string(firstSelStart);
                    SetSelectionsFromStartEnds({se});
                }
            }
        }
        if (newInfo->tv) {
            if (openTreeViewer && openTreeViewer != newInfo->tv) [openTreeViewer hidePanel];
            [newInfo->tv showPanel];
            openTreeViewer = newInfo->tv;
        } else if (openTreeViewer) {
            [openTreeViewer hidePanel];
        }
    } else if (openTreeViewer) {
        [openTreeViewer hidePanel];
    }
    // grepper tree and per-file tree never visible at the same time
    JTGrepperForm* grepperForm = [JTGrepperForm shared];
    if (grepperForm && [grepperForm tv] && !grepperTreeViewJustOpened) {
        if (GetCurrentFilePath() == [[grepperForm tv] fname]) {
            if (openTreeViewer) [openTreeViewer hidePanel];
            [[grepperForm tv] showPanel];
        } else {
            [[grepperForm tv] hidePanel];
            if (openTreeViewer && newFileHasInfo && newInfo->tv == openTreeViewer)
                [openTreeViewer showPanel];
        }
    }
    grepperTreeViewJustOpened = false;
    activeFname = newFname;
    if (!jtSettings().auto_validate) ValidateIfFilenameMatches(newFname);
    if (newFileHasInfo && !newInfo->statusBarSection.empty())
        SetStatusBarSection(newInfo->statusBarSection);
}

void OnFileClosed(uintptr_t bufferId) {
    std::string bufferClosed;
    auto mapIt = sBufferIdToFname.find(bufferId);
    if (mapIt != sBufferIdToFname.end()) {
        bufferClosed = mapIt->second;
        sBufferIdToFname.erase(mapIt);
    } else {
        bufferClosed = GetFilePathFromBufferId(bufferId); // best effort
    }
    if (bufferClosed.empty()) return;
    JsonFileInfo* closedInfo = nullptr;
    if (TryGetInfoForFile(bufferClosed, &closedInfo)) {
        if (openTreeViewer && openTreeViewer == closedInfo->tv) openTreeViewer = nil;
        DisposeInfo(*closedInfo);
        jsonFileInfos.erase(bufferClosed);
    }
    JTGrepperForm* grepperForm = [JTGrepperForm shared];
    if (grepperForm && [grepperForm tv] && bufferClosed == [[grepperForm tv] fname]) {
        [[grepperForm tv] closePanel];
    }
}

void OnFileBeforeRenameOrSave(uintptr_t bufferId) {
    std::string bufferOldName = GetFilePathFromBufferId(bufferId);
    sJsonFilesRenamed[bufferId] = bufferOldName;
    JTGrepperForm* grepperForm = [JTGrepperForm shared];
    if (grepperForm && [grepperForm tv] && [[grepperForm tv] fname] == bufferOldName)
        sShouldRenameGrepperForm = true;
}

void OnFileRenamedOrSaved(uintptr_t bufferId) {
    std::string bufferNewName = GetFilePathFromBufferId(bufferId);
    if (bufferNewName == sSchemasToFnamePatternsFname) ParseSchemasToFnamePatternsFile();
    auto it = sJsonFilesRenamed.find(bufferId);
    if (it != sJsonFilesRenamed.end()) {
        std::string bufferOldName = it->second;
        sJsonFilesRenamed.erase(it);
        if (activeFname == bufferOldName) activeFname = bufferNewName;
        auto infoIt = jsonFileInfos.find(bufferOldName);
        if (infoIt != jsonFileInfos.end() && bufferOldName != bufferNewName) {
            JsonFileInfo moved = infoIt->second;
            jsonFileInfos.erase(infoIt);
            if (moved.tv) [moved.tv renameTo:bufferNewName isFilename:true];
            jsonFileInfos[bufferNewName] = moved;
        }
    }
    JTGrepperForm* grepperForm = [JTGrepperForm shared];
    if (sShouldRenameGrepperForm) {
        if (grepperForm && [grepperForm tv]) [[grepperForm tv] setFname:bufferNewName];
        sShouldRenameGrepperForm = false;
    }
    ValidateIfFilenameMatches(bufferNewName);
}

void OnFileRenameCancel(uintptr_t bufferId) {
    sJsonFilesRenamed.erase(bufferId);
    sShouldRenameGrepperForm = false;
}

void OnModified() {
    sLastEditedTime = CFAbsoluteTimeGetCurrent();
    if (openTreeViewer) openTreeViewer.shouldRefresh = true;
}

void OnThemeChanged() {
    jtErrorFormApplyStyling();
    jtSortFormApplyStyling();
    jtRegexSearchFormApplyStyling();
    JTGrepperForm* grepperForm = [JTGrepperForm shared];
    if (grepperForm) [grepperForm applyStyling];
    for (auto& [fname, info] : jsonFileInfos) {
        if (info.tv) [info.tv applyStyling];
    }
}

void TimerTick() {
    const JtSettings& s = jtSettings();
    if (!s.auto_validate || !sBufferFinishedOpening) return;
    int64_t len = DocLength();
    if (len > s.max_file_size_MB_slow_actions * 1e6) return;
    if (sLastEditedTime == DBL_MAX) return;
    double waitSeconds = s.inactivity_seconds_before_parse < 1
        ? 1.0 : (double)s.inactivity_seconds_before_parse;
    if (sLastEditedTime + waitSeconds > CFAbsoluteTimeGetCurrent()) return;
    sLastEditedTime = DBL_MAX;
    std::string fname = GetCurrentFilePath();
    std::string ext = FileExtension(fname);
    JsonFileInfo* info = nullptr;
    if ((TryGetInfoForFile(fname, &info) &&
         (info->documentType == DocumentType::INI || info->documentType == DocumentType::REGEX ||
          info->usesSelections)) ||
        ValidateIfFilenameMatches(fname, true) || !IsAutoParseExtension(ext))
        return;
    TryParseJson(ext == "jsonl" ? DocumentType::JSONL : DocumentType::JSON, true, false, false,
                 true);
    RefreshErrorFormIfShown(fname, true);
}

void PluginCleanUp() {
    jtSettings().auto_validate = false;
    JTGrepperForm* grepperForm = [JTGrepperForm shared];
    if (grepperForm) [grepperForm closeForm];
    for (auto& [fname, info] : jsonFileInfos) DisposeInfo(info);
    jsonFileInfos.clear();
    openTreeViewer = nil;
    jtErrorFormShutdown();
    WriteSchemasToFnamePatternsFile(sSchemasToFnamePatterns);
    jtSettingsSave();
}

} // namespace jtcore
