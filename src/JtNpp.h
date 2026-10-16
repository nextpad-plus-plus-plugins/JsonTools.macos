// JsonTools.macos — JtNpp.h: the Npp.cs / SelectionManager.cs helper layer.
// Thin wrappers over the host + Scintilla message surface. All positions are
// UTF-8 byte offsets (the engine is UTF-8 native, same as Scintilla).

#pragma once

#import <Cocoa/Cocoa.h>

#include "NppPluginInterfaceMac.h"

#include <string>
#include <utility>
#include <vector>

extern NppData nppData;

namespace jtnpp {

// LangType values (Notepad_plus_msgs.h)
constexpr int L_INI = 13;
constexpr int L_YAML = 49;
constexpr int L_JSON = 57;

intptr_t Npp(uint32_t msg, uintptr_t w = 0, intptr_t l = 0);
// messages the active Scintilla view
intptr_t Sci(uint32_t msg, uintptr_t w = 0, intptr_t l = 0);
// re-resolve which Scintilla view is active (call on NPPN_BUFFERACTIVATED)
void RefreshCurrentScintilla();

int64_t DocLength();
std::string GetText();                      // whole document
std::string GetSlice(int64_t start, int64_t end);
void SetText(const std::string& s);
void AppendText(const std::string& s);
void DeleteRange(int64_t start, int64_t length);
void InsertText(int64_t pos, const std::string& s);
void BeginUndoAction();
void EndUndoAction();
int64_t GetCurrentPos();
void GotoPos(int64_t pos);
void GoToLegalPos(int64_t pos); // clamps into [0, len] and scrolls caret
void ScrollCaret();
void GrabFocus();
int64_t LineFromPosition(int64_t pos);
void EnsureLineVisible(int64_t line); // unhide + unfold + scroll

std::string GetCurrentFilePath();
std::string GetFilePathFromBufferId(uintptr_t bufferId);
void FileNew();
void OpenFile(const std::string& path);
void SetCurrentLang(int langType);
std::string ConfigDirectory();              // plugins config dir
std::string ConfigSubDirectory();           // <config>/JsonTools, created on demand
void SetStatusBarSection(const std::string& text);
void TryCopyToClipboard(const std::string& s);
void OpenUrlInWebBrowser(const std::string& url);

// file extension (lowercased, no dot) of fname, or of the current file
std::string FileExtension(const std::string& fname);
std::string CurrentFileExtension();

// ── message boxes (Translator.ShowTranslatedMessageBox; English only) ──────
void MsgBoxInfo(const std::string& caption, const std::string& msg);
void MsgBoxError(const std::string& caption, const std::string& msg);
void MsgBoxWarning(const std::string& caption, const std::string& msg);
// returns true for Yes / OK
bool MsgBoxYesNo(const std::string& caption, const std::string& msg);
bool MsgBoxOkCancel(const std::string& caption, const std::string& msg);

// ── selections (SelectionManager.cs) ────────────────────────────────────────
std::vector<std::pair<int64_t, int64_t>> GetSelectedRanges();
bool NoTextSelected(const std::vector<std::pair<int64_t, int64_t>>& sels);
// keys like "start,end"
bool IsStartEnd(const std::string& s);
bool ParseStartEnd(const std::string& s, int64_t& start, int64_t& end);
int StartEndCompareByStart(const std::string& a, const std::string& b);
int64_t StartFromStartEnd(const std::string& s);
std::vector<std::pair<int64_t, int64_t>>
SetSelectionsFromStartEnds(const std::vector<std::string>& startEnds);
void SetSelection(int64_t start, int64_t end);
void AddSelection(int64_t start, int64_t end);
void ClearSelections();

// ── indicators (selection remembering) ──────────────────────────────────────
extern int gSelRememberIndicator1;
extern int gSelRememberIndicator2;
void AllocateSelectionIndicators();
void HideSelectionRememberingIndicators();
int64_t GetIndicatorCurrent();
void SetIndicatorCurrent(int64_t indicator);
void IndicatorFillRange(int64_t start, int64_t length);
void IndicatorClearRange(int64_t start, int64_t length);
// sorted "start,end" strings of all regions marked with either indicator
std::vector<std::string> GetRegionsWithIndicator();
// the remembered region enclosing pos, or (-1,-1)
std::pair<int64_t, int64_t> GetEnclosingRememberedSelection(int64_t pos);

} // namespace jtnpp
