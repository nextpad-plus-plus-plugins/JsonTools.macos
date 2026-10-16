// JsonTools.macos — JtNpp.mm: see JtNpp.h.

#import <Cocoa/Cocoa.h>

#include "JtNpp.h"

#include "Scintilla.h"

#include <algorithm>
#include <cstring>

namespace jtnpp {

static uintptr_t sCurrentSci = 0;

intptr_t Npp(uint32_t msg, uintptr_t w, intptr_t l) {
    return nppData._sendMessage(nppData._nppHandle, msg, w, l);
}

void RefreshCurrentScintilla() {
    int which = 0;
    Npp(NPPM_GETCURRENTSCINTILLA, 0, (intptr_t)&which);
    sCurrentSci = which == 1 ? nppData._scintillaSecondHandle : nppData._scintillaMainHandle;
}

intptr_t Sci(uint32_t msg, uintptr_t w, intptr_t l) {
    if (sCurrentSci == 0) RefreshCurrentScintilla();
    return nppData._sendMessage(sCurrentSci, msg, w, l);
}

int64_t DocLength() { return (int64_t)Sci(SCI_GETLENGTH); }

std::string GetText() {
    int64_t len = DocLength();
    std::string s(static_cast<size_t>(len), '\0');
    if (len > 0) Sci(SCI_GETTEXT, (uintptr_t)(len + 1), (intptr_t)&s[0]);
    return s;
}

std::string GetSlice(int64_t start, int64_t end) {
    if (end <= start) return "";
    std::string s(static_cast<size_t>(end - start), '\0');
    Sci_TextRangeFull tr;
    tr.chrg.cpMin = start;
    tr.chrg.cpMax = end;
    tr.lpstrText = &s[0];
    Sci(SCI_GETTEXTRANGEFULL, 0, (intptr_t)&tr);
    return s;
}

void SetText(const std::string& s) { Sci(SCI_SETTEXT, 0, (intptr_t)s.c_str()); }
void AppendText(const std::string& s) { Sci(SCI_APPENDTEXT, s.size(), (intptr_t)s.data()); }
void DeleteRange(int64_t start, int64_t length) { Sci(SCI_DELETERANGE, (uintptr_t)start, length); }
void InsertText(int64_t pos, const std::string& s) { Sci(SCI_INSERTTEXT, (uintptr_t)pos, (intptr_t)s.c_str()); }
void BeginUndoAction() { Sci(SCI_BEGINUNDOACTION); }
void EndUndoAction() { Sci(SCI_ENDUNDOACTION); }
int64_t GetCurrentPos() { return (int64_t)Sci(SCI_GETCURRENTPOS); }
void GotoPos(int64_t pos) { Sci(SCI_GOTOPOS, (uintptr_t)pos); }

void GoToLegalPos(int64_t pos) {
    int64_t len = DocLength();
    if (pos < 0) pos = 0;
    if (pos > len) pos = len;
    Sci(SCI_GOTOPOS, (uintptr_t)pos);
    Sci(SCI_SCROLLCARET);
}

void ScrollCaret() { Sci(SCI_SCROLLCARET); }
void GrabFocus() { Sci(SCI_GRABFOCUS); }
int64_t LineFromPosition(int64_t pos) { return (int64_t)Sci(SCI_LINEFROMPOSITION, (uintptr_t)pos); }

void EnsureLineVisible(int64_t line) {
    // undo "hide lines" if the line is hidden, then unfold, then scroll
    if (!Sci(SCI_GETLINEVISIBLE, (uintptr_t)line)) {
        // find the hidden range around this line and show it
        int64_t firstHidden = line, lastHidden = line;
        while (firstHidden > 0 && !Sci(SCI_GETLINEVISIBLE, (uintptr_t)(firstHidden - 1)))
            firstHidden--;
        int64_t lineCount = (int64_t)Sci(SCI_GETLINECOUNT);
        while (lastHidden < lineCount - 1 && !Sci(SCI_GETLINEVISIBLE, (uintptr_t)(lastHidden + 1)))
            lastHidden++;
        Sci(SCI_SHOWLINES, (uintptr_t)firstHidden, lastHidden);
    }
    Sci(SCI_ENSUREVISIBLE, (uintptr_t)line);
}

std::string GetCurrentFilePath() {
    char buf[4096] = {0};
    Npp(NPPM_GETFULLCURRENTPATH, sizeof(buf) - 1, (intptr_t)buf);
    return buf;
}

std::string GetFilePathFromBufferId(uintptr_t bufferId) {
    char buf[4096] = {0};
    Npp(NPPM_GETFULLPATHFROMBUFFERID, bufferId, (intptr_t)buf);
    return buf;
}

void FileNew() {
    Npp(NPPM_MENUCOMMAND, 0, 41001 /* IDM_FILE_NEW */);
    RefreshCurrentScintilla();
}

void OpenFile(const std::string& path) { Npp(NPPM_DOOPEN, 0, (intptr_t)path.c_str()); }
void SetCurrentLang(int langType) { Npp(NPPM_SETCURRENTLANGTYPE, 0, langType); }

std::string ConfigDirectory() {
    char buf[2048] = {0};
    Npp(NPPM_GETPLUGINSCONFIGDIR, sizeof(buf) - 1, (intptr_t)buf);
    return buf;
}

std::string ConfigSubDirectory() {
    std::string dir = ConfigDirectory();
    if (dir.empty()) return "";
    NSString* sub = [NSString stringWithFormat:@"%s/JsonTools", dir.c_str()];
    [[NSFileManager defaultManager] createDirectoryAtPath:sub
                              withIntermediateDirectories:YES
                                               attributes:nil
                                                    error:nil];
    return std::string(sub.UTF8String);
}

void SetStatusBarSection(const std::string& text) {
    Npp(NPPM_SETSTATUSBAR, STATUSBAR_DOC_TYPE, (intptr_t)text.c_str());
}

void TryCopyToClipboard(const std::string& s) {
    NSPasteboard* pb = [NSPasteboard generalPasteboard];
    [pb clearContents];
    [pb setString:([NSString stringWithUTF8String:s.c_str()] ?: @"") forType:NSPasteboardTypeString];
}

void OpenUrlInWebBrowser(const std::string& url) {
    NSString* s = [NSString stringWithUTF8String:url.c_str()];
    NSURL* u = s ? [NSURL URLWithString:s] : nil;
    if (u) [[NSWorkspace sharedWorkspace] openURL:u];
}

std::string FileExtension(const std::string& fname) {
    size_t dot = fname.rfind('.');
    size_t slash = fname.rfind('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
    std::string ext = fname.substr(dot + 1);
    for (char& c : ext) c = (char)tolower((unsigned char)c);
    return ext;
}

std::string CurrentFileExtension() { return FileExtension(GetCurrentFilePath()); }

// ── message boxes ────────────────────────────────────────────────────────────
static NSModalResponse RunAlert(const std::string& caption, const std::string& msg,
                                NSAlertStyle style, NSString* first, NSString* second) {
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = [NSString stringWithUTF8String:caption.c_str()] ?: @"JsonTools";
    alert.informativeText = [NSString stringWithUTF8String:msg.c_str()] ?: @"";
    alert.alertStyle = style;
    [alert addButtonWithTitle:first];
    if (second) [alert addButtonWithTitle:second];
    return [alert runModal];
}

void MsgBoxInfo(const std::string& caption, const std::string& msg) {
    RunAlert(caption, msg, NSAlertStyleInformational, @"OK", nil);
}
void MsgBoxError(const std::string& caption, const std::string& msg) {
    RunAlert(caption, msg, NSAlertStyleCritical, @"OK", nil);
}
void MsgBoxWarning(const std::string& caption, const std::string& msg) {
    RunAlert(caption, msg, NSAlertStyleWarning, @"OK", nil);
}
bool MsgBoxYesNo(const std::string& caption, const std::string& msg) {
    return RunAlert(caption, msg, NSAlertStyleInformational, @"Yes", @"No") ==
           NSAlertFirstButtonReturn;
}
bool MsgBoxOkCancel(const std::string& caption, const std::string& msg) {
    return RunAlert(caption, msg, NSAlertStyleWarning, @"OK", @"Cancel") ==
           NSAlertFirstButtonReturn;
}

// ── selections ───────────────────────────────────────────────────────────────
std::vector<std::pair<int64_t, int64_t>> GetSelectedRanges() {
    std::vector<std::pair<int64_t, int64_t>> out;
    int64_t n = (int64_t)Sci(SCI_GETSELECTIONS);
    for (int64_t i = 0; i < n; i++) {
        int64_t start = (int64_t)Sci(SCI_GETSELECTIONNSTART, (uintptr_t)i);
        int64_t end = (int64_t)Sci(SCI_GETSELECTIONNEND, (uintptr_t)i);
        out.emplace_back(start, end);
    }
    if (out.empty()) out.emplace_back(GetCurrentPos(), GetCurrentPos());
    return out;
}

bool NoTextSelected(const std::vector<std::pair<int64_t, int64_t>>& sels) {
    return sels.size() == 1 && sels[0].first == sels[0].second;
}

bool IsStartEnd(const std::string& s) {
    int64_t a, b;
    return ParseStartEnd(s, a, b);
}

bool ParseStartEnd(const std::string& s, int64_t& start, int64_t& end) {
    size_t comma = s.find(',');
    if (comma == std::string::npos || comma == 0 || comma == s.size() - 1) return false;
    for (size_t i = 0; i < s.size(); i++)
        if (i != comma && !isdigit((unsigned char)s[i])) return false;
    start = strtoll(s.c_str(), nullptr, 10);
    end = strtoll(s.c_str() + comma + 1, nullptr, 10);
    return true;
}

int64_t StartFromStartEnd(const std::string& s) {
    return strtoll(s.c_str(), nullptr, 10);
}

int StartEndCompareByStart(const std::string& a, const std::string& b) {
    int64_t sa = StartFromStartEnd(a), sb = StartFromStartEnd(b);
    return sa < sb ? -1 : sa > sb ? 1 : 0;
}

void SetSelection(int64_t start, int64_t end) {
    Sci(SCI_SETSELECTION, (uintptr_t)start, end);
}

void AddSelection(int64_t start, int64_t end) {
    Sci(SCI_ADDSELECTION, (uintptr_t)start, end);
}

void ClearSelections() { Sci(SCI_CLEARSELECTIONS); }

std::vector<std::pair<int64_t, int64_t>>
SetSelectionsFromStartEnds(const std::vector<std::string>& startEnds) {
    std::vector<std::pair<int64_t, int64_t>> ranges;
    for (const std::string& se : startEnds) {
        int64_t start, end;
        if (ParseStartEnd(se, start, end)) ranges.emplace_back(start, end);
    }
    std::sort(ranges.begin(), ranges.end());
    bool first = true;
    for (auto& [start, end] : ranges) {
        if (first)
            SetSelection(start, end);
        else
            AddSelection(start, end);
        first = false;
    }
    if (ranges.empty()) return GetSelectedRanges();
    return ranges;
}

// ── indicators ───────────────────────────────────────────────────────────────
int gSelRememberIndicator1 = 12;
int gSelRememberIndicator2 = 13;
static bool sIndicatorsAllocated = false;

void AllocateSelectionIndicators() {
    if (sIndicatorsAllocated) return;
    int firstIndicator = 0;
    if (Npp(NPPM_ALLOCATEINDICATOR, 2, (intptr_t)&firstIndicator) && firstIndicator > 0) {
        gSelRememberIndicator1 = firstIndicator;
        gSelRememberIndicator2 = firstIndicator + 1;
    }
    sIndicatorsAllocated = true;
}

void HideSelectionRememberingIndicators() {
    for (uintptr_t h : {nppData._scintillaMainHandle, nppData._scintillaSecondHandle}) {
        for (int indicator : {gSelRememberIndicator1, gSelRememberIndicator2}) {
            if (indicator >= 0)
                nppData._sendMessage(h, SCI_INDICSETSTYLE, (uintptr_t)indicator, INDIC_HIDDEN);
        }
    }
}

int64_t GetIndicatorCurrent() { return (int64_t)Sci(SCI_GETINDICATORCURRENT); }
void SetIndicatorCurrent(int64_t indicator) { Sci(SCI_SETINDICATORCURRENT, (uintptr_t)indicator); }
void IndicatorFillRange(int64_t start, int64_t length) {
    Sci(SCI_INDICATORFILLRANGE, (uintptr_t)start, length);
}
void IndicatorClearRange(int64_t start, int64_t length) {
    Sci(SCI_INDICATORCLEARRANGE, (uintptr_t)start, length);
}

static void RegionsForIndicator(int indicator, std::vector<std::pair<int64_t, int64_t>>& out) {
    int64_t len = DocLength();
    int64_t pos = 0;
    while (pos < len) {
        bool on = Sci(SCI_INDICATORVALUEAT, (uintptr_t)indicator, pos) != 0;
        int64_t end = (int64_t)Sci(SCI_INDICATOREND, (uintptr_t)indicator, pos);
        if (end <= pos) break; // no more transitions
        if (on) out.emplace_back(pos, end);
        pos = end;
    }
}

std::vector<std::string> GetRegionsWithIndicator() {
    std::vector<std::pair<int64_t, int64_t>> regions;
    RegionsForIndicator(gSelRememberIndicator1, regions);
    RegionsForIndicator(gSelRememberIndicator2, regions);
    std::sort(regions.begin(), regions.end());
    std::vector<std::string> out;
    for (auto& [start, end] : regions)
        out.push_back(std::to_string(start) + "," + std::to_string(end));
    return out;
}

std::pair<int64_t, int64_t> GetEnclosingRememberedSelection(int64_t pos) {
    for (int indicator : {gSelRememberIndicator1, gSelRememberIndicator2}) {
        if (Sci(SCI_INDICATORVALUEAT, (uintptr_t)indicator, pos)) {
            int64_t start = (int64_t)Sci(SCI_INDICATORSTART, (uintptr_t)indicator, pos);
            int64_t end = (int64_t)Sci(SCI_INDICATOREND, (uintptr_t)indicator, pos);
            return {start, end};
        }
    }
    return {-1, -1};
}

} // namespace jtnpp
