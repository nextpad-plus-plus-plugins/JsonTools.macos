// JsonTools.macos — macOS port of molsonkiko's JsonTools Notepad++ plugin
// (https://github.com/molsonkiko/JsonToolsNppPlugin — C#/WinForms; this port
// is a full rewrite: pure-C++ engine + Cocoa shell against the Nextpad++
// macOS plugin API).
//
// PluginEntry.mm — plugin contract: the 31 menu items (1:1 with the Windows
// menu, Main.CommandMenuInit), notification routing, the auto-parse timer,
// and the settings lifecycle.

#import <Cocoa/Cocoa.h>

#include "NppPluginInterfaceMac.h"
#include "Scintilla.h"

#include "JtCore.h"
#include "JtNpp.h"
#include "JtSettings.h"

#define PLUGIN_NAME "JsonTools"
static const int NB_FUNC = 31;

NppData nppData;
static FuncItem funcItem[NB_FUNC];
static ShortcutKey shortcuts[8];
static NSTimer* sParseTimer = nil;

// FuncItem indices that double as docking dlgIDs (TreeViewer.mm / JtForms.mm)
extern const int JT_TREE_FUNC_INDEX = 7;
extern const int JT_ERROR_FORM_FUNC_INDEX = 19;

int jtFuncItemCmdId(int funcItemIndex) {
    if (funcItemIndex < 0 || funcItemIndex >= NB_FUNC) return 0;
    return funcItem[funcItemIndex]._cmdID;
}

using namespace jtcore;

// plain functions for the FuncItem table
static void cmdDocs() { CmdDocs(); }
static void cmdPrettyPrint() { CmdPrettyPrintJson(); }
static void cmdCompress() { CmdCompressJson(); }
static void cmdPathToPosition() { CmdCopyPathToCurrentPosition(); }
static void cmdSelectEveryValidJson() { CmdSelectEveryValidJson(); }
static void cmdCheckSyntax() { CmdCheckJsonSyntaxNow(); }
static void cmdOpenTree() { CmdOpenJsonTree(); }
static void cmdGrepper() { CmdOpenGrepperForm(); }
static void cmdSortForm() { CmdOpenSortForm(); }
static void cmdSettings() { CmdOpenSettings(); }
static void cmdValidate() { CmdValidateJson(); }
static void cmdMapSchemas() { CmdMapSchemasToFnamePatterns(); }
static void cmdGenerateSchema() { CmdGenerateJsonSchema(); }
static void cmdGenerateRandom() { CmdGenerateRandomJson(); }
static void cmdRunTests() { CmdRunTests(); }
static void cmdAbout() { CmdShowAboutForm(); }
static void cmdErrorForm() { CmdOpenErrorForm(); }
static void cmdDumpYaml() { CmdDumpYaml(); }
static void cmdTreeJsonLines() { CmdOpenJsonTreeJsonLines(); }
static void cmdDumpJsonLines() { CmdDumpJsonLines(); }
static void cmdDumpSelectedAsString() { CmdDumpSelectedTextAsJsonString(); }
static void cmdDumpStringsAsText() { CmdDumpSelectedJsonStringsAsText(); }
static void cmdTreeIni() { CmdOpenJsonTreeIni(); }
static void cmdRegexSearch() { CmdRegexSearchToJson(); }

extern "C" NPP_EXPORT void setInfo(NppData data) {
    nppData = data;
    jtSettingsLoad();

    int idx = 0;
    auto addItem = [&](const char* name, PFUNCPLUGINCMD func, ShortcutKey* shortcut = nullptr) {
        strlcpy(funcItem[idx]._itemName, name, NPP_MENU_ITEM_SIZE);
        funcItem[idx]._pFunc = func;
        funcItem[idx]._init2Check = false;
        funcItem[idx]._pShKey = shortcut;
        idx++;
    };
    // Windows used Ctrl+Alt+Shift+<key>; macOS: ⌘⌥⇧<key>
    bool useShortcuts = jtSettings().keyboard_shortcuts;
    auto sk = [&](int i, UCHAR key) -> ShortcutKey* {
        if (!useShortcuts) return nullptr;
        shortcuts[i] = {false, true, true, true, key};
        return &shortcuts[i];
    };

    // 1:1 with the Windows plugin's menu (Main.CommandMenuInit)
    addItem("Documentation", cmdDocs);                                         // 0
    addItem("Pretty-print current JSON file", cmdPrettyPrint, sk(0, 'P'));     // 1
    addItem("Compress current JSON file", cmdCompress, sk(1, 'C'));            // 2
    addItem("Path to current position", cmdPathToPosition, sk(2, 'L'));        // 3
    addItem("Select every valid JSON in selection", cmdSelectEveryValidJson);  // 4
    addItem("Check JSON syntax now", cmdCheckSyntax);                          // 5
    addItem("", nullptr);                                                      // 6 ---
    addItem("Open JSON tree viewer", cmdOpenTree, sk(3, 'J'));                 // 7
    addItem("Get JSON from files and APIs", cmdGrepper, sk(4, 'G'));           // 8
    addItem("Sort arrays", cmdSortForm);                                       // 9
    addItem("Settings", cmdSettings, sk(5, 'S'));                              // 10
    addItem("", nullptr);                                                      // 11 ---
    addItem("Validate JSON against JSON schema", cmdValidate);                 // 12
    addItem("Validate files with JSON schema if name matches pattern", cmdMapSchemas); // 13
    addItem("Generate schema from JSON", cmdGenerateSchema);                   // 14
    addItem("Generate random JSON from schema", cmdGenerateRandom);            // 15
    addItem("", nullptr);                                                      // 16 ---
    addItem("Run tests", cmdRunTests);                                         // 17
    addItem("About", cmdAbout);                                                // 18
    addItem("See most recent syntax errors in this file", cmdErrorForm);       // 19
    addItem("JSON to YAML", cmdDumpYaml);                                      // 20
    addItem("", nullptr);                                                      // 21 ---
    addItem("Parse JSON Lines document", cmdTreeJsonLines);                    // 22
    addItem("Array to JSON Lines", cmdDumpJsonLines);                          // 23
    addItem("", nullptr);                                                      // 24 ---
    addItem("Dump selected text as JSON string(s)", cmdDumpSelectedAsString);  // 25
    addItem("Dump JSON string(s) as raw text", cmdDumpStringsAsText);          // 26
    addItem("", nullptr);                                                      // 27 ---
    addItem("Open tree for INI file", cmdTreeIni);                             // 28
    addItem("", nullptr);                                                      // 29 ---
    addItem("Regex search to JSON", cmdRegexSearch);                           // 30
}

extern "C" NPP_EXPORT const char* getName() {
    return PLUGIN_NAME;
}

extern "C" NPP_EXPORT FuncItem* getFuncsArray(int* nbF) {
    *nbF = NB_FUNC;
    return funcItem;
}

extern "C" NPP_EXPORT void beNotified(SCNotification* n) {
    switch (n->nmhdr.code) {
        case NPPN_READY:
            OnReady();
            // 1-second auto-parse timer (Main.parseTimer)
            if (!sParseTimer) {
                sParseTimer = [NSTimer scheduledTimerWithTimeInterval:1.0
                                                              repeats:YES
                                                                block:^(NSTimer* t) {
                                                                  jtcore::TimerTick();
                                                                }];
            }
            break;

        case NPPN_TBMODIFICATION:
            // Main.SetToolBarIcons — the host loads <plugins>/JsonTools/toolbar.png;
            // one icon (the tree toggle) stands in for the Windows set
            jtnpp::Npp(NPPM_ADDTOOLBARICON_FORDARKMODE,
                       (uintptr_t)funcItem[JT_TREE_FUNC_INDEX]._cmdID, (intptr_t)"toolbar.png");
            break;

        case NPPN_FILEBEFOREOPEN:
            OnFileBeforeOpen();
            break;

        case NPPN_BUFFERACTIVATED:
            OnBufferActivated((uintptr_t)n->nmhdr.idFrom);
            break;

        case NPPN_FILECLOSED: // this host sends FILECLOSED, not FILEBEFORECLOSE
            OnFileClosed((uintptr_t)n->nmhdr.idFrom);
            break;

        case NPPN_WORDSTYLESUPDATED:
        case NPPN_DARKMODECHANGED:
            OnThemeChanged();
            break;

        case NPPN_FILEBEFORESAVE:
        case NPPN_FILEBEFORERENAME:
            OnFileBeforeRenameOrSave((uintptr_t)n->nmhdr.idFrom);
            break;

        case NPPN_FILESAVED:
        case NPPN_FILERENAMED:
            OnFileRenamedOrSaved((uintptr_t)n->nmhdr.idFrom);
            break;

        case NPPN_FILERENAMECANCEL:
            OnFileRenameCancel((uintptr_t)n->nmhdr.idFrom);
            break;

        case SCN_MODIFIED:
            if ((n->modificationType & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT)) != 0) OnModified();
            break;

        case NPPN_GLOBALMODIFIED:
            OnModified();
            break;

        case NPPN_SHUTDOWN:
            if (sParseTimer) {
                [sParseTimer invalidate];
                sParseTimer = nil;
            }
            PluginCleanUp();
            break;

        default:
            break;
    }
}

extern "C" NPP_EXPORT intptr_t messageProc(uint32_t, uintptr_t, intptr_t) {
    return 1;
}
