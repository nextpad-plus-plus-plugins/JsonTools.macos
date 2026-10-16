// JsonTools.macos — JtCore.h: port of Main.cs (minus the plugin contract,
// which lives in PluginEntry.mm). Owns the per-file JSON map, parsing,
// reformatting, validation, the schema cache, and the auto-parse timer tick.

#pragma once

#import <Cocoa/Cocoa.h>

#include <map>
#include <string>
#include <vector>

#include "JNode.h"
#include "JsonParser.h"

@class JTTreeViewer;

namespace jtcore {

using jsontools::Comment;
using jsontools::DocumentType;
using jsontools::JNodePtr;
using jsontools::JsonLint;
using jsontools::KeyStyle;
using jsontools::ParserState;

struct JsonFileInfo {
    JNodePtr json;
    std::vector<JsonLint> lints;
    bool hasLints = false;
    std::string statusBarSection;
    JTTreeViewer* __strong tv = nil;
    // if true, json maps "start,end" selection keys to the JSON at each selection
    bool usesSelections = false;
    std::vector<Comment> comments;
    bool hasComments = false;
    DocumentType documentType = DocumentType::JSON;
    // the last schema file used to validate this file ("" = none)
    std::string filenameOfMostRecentValidatingSchema;
};

struct ParseResult {
    ParserState parserState;
    JNodePtr node; // nullptr when parse was skipped/fatal with no result
    bool usesSelections;
    DocumentType documentType;
};

// ── state (Main.cs fields) ──────────────────────────────────────────────────
extern std::string activeFname;
extern std::map<std::string, JsonFileInfo> jsonFileInfos;
extern JTTreeViewer* openTreeViewer;
extern char pathSeparator;
extern bool errorFormTriggeredParse;
extern bool grepperTreeViewJustOpened;

bool TryGetInfoForFile(const std::string& fname, JsonFileInfo** info);
JsonFileInfo& AddJsonForFile(const std::string& fname, const JNodePtr& json);

// ── parsing / reformatting ──────────────────────────────────────────────────
jsontools::JsonParser JsonParserFromSettings();
ParseResult TryParseJson(DocumentType documentType = DocumentType::JSON,
                         bool wasAutotriggered = false, bool preferPreviousDocumentType = false,
                         bool isRecursion = false, bool ignoreSelections = false);
std::string PrettyPrintFromSettings(const JNodePtr& json);
std::string ToJsonLinesFromSettings(const JNodePtr& json);
void PrettyPrintJsonInNewFile(const JNodePtr& json);
// returns oldKey -> (newKey, child) selection-key changes
std::map<std::string, std::pair<std::string, JNodePtr>>
ReformatFileWithJson(const JNodePtr& json,
                     const std::function<std::string(const JNodePtr&)>& formatter,
                     bool usesSelections);
void RenameAll(const std::map<std::string, std::pair<std::string, JNodePtr>>& keyChanges,
               jsontools::JObject* obj);
int64_t EndOfJNodeAtPos(int64_t startUtf8Pos, int64_t end);
void SelectAllChildren(std::vector<int64_t> positions, bool isJsonLines);
std::string PathToPosition(KeyStyle style, char separator, int64_t pos = -1);
void SetPathSeparatorFromSettings();

// ── menu commands ───────────────────────────────────────────────────────────
void CmdDocs();
void CmdPrettyPrintJson();
void CmdCompressJson();
void CmdCopyPathToCurrentPosition();
void CmdSelectEveryValidJson();
void CmdCheckJsonSyntaxNow();
void CmdOpenJsonTree(DocumentType documentType = DocumentType::JSON);
void CmdOpenGrepperForm();
void CmdOpenSortForm();
void CmdOpenSettings();
void CmdValidateJson();
void CmdMapSchemasToFnamePatterns();
void CmdGenerateJsonSchema();
void CmdGenerateRandomJson();
void CmdRunTests();
void CmdShowAboutForm();
void CmdOpenErrorForm(); // "See most recent syntax errors in this file"
void CmdDumpYaml();
void CmdOpenJsonTreeJsonLines();
void CmdDumpJsonLines();
void CmdDumpSelectedTextAsJsonString();
void CmdDumpSelectedJsonStringsAsText();
void CmdOpenJsonTreeIni();
void CmdRegexSearchToJson();

// validation (also used by the error form's refresh)
void ValidateJson(std::string schemaPath, bool messageOnSuccess, bool ignoreSelections,
                  bool wasAutotriggered);
bool ValidateIfFilenameMatches(const std::string& fname, bool wasAutotriggered = false);
void OpenErrorForm(const std::string& fname, bool wasAutoTriggered);
void RefreshErrorFormIfShown(const std::string& fname, bool suppressIfFormNotVisible);

// used by TreeViewer / forms
void DisplayJsonTree(JTTreeViewer* treeViewer, const JNodePtr& json, const std::string& title,
                     bool usesSelections, DocumentType documentType, bool fatalParserError);

// ── notifications (called from PluginEntry) ─────────────────────────────────
void OnReady();
void OnBufferActivated(uintptr_t bufferId);
void OnFileBeforeOpen();
// the host emits NPPN_FILECLOSED (not FILEBEFORECLOSE); the editor is already
// out of the tab list by then, so the fname comes from our own id→fname map
void OnFileClosed(uintptr_t bufferId);
void OnFileBeforeRenameOrSave(uintptr_t bufferId);
void OnFileRenamedOrSaved(uintptr_t bufferId);
void OnFileRenameCancel(uintptr_t bufferId);
void OnModified();     // text insert/delete happened in the active buffer
void OnThemeChanged(); // NPPN_DARKMODECHANGED / WORDSTYLESUPDATED equivalent
void TimerTick();      // 1-second auto-parse timer (DelayedParseAfterEditing)
void PluginCleanUp();

} // namespace jtcore
