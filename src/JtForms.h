// JsonTools.macos — JtForms.h: the plugin's dialogs and the error-list panel.
// Ports of AboutForm, ErrorForm (bottom dock), SortForm, JsonToCsvForm,
// RegexSearchForm, FindReplaceForm, and the Settings dialog.

#pragma once

#import <Cocoa/Cocoa.h>

#include <string>
#include <vector>

#include "JNode.h"
#include "JsonParser.h"

@class JTTreeViewer;

// About box (modal)
void jtShowAboutDialog(void);

// Settings dialog (modal); returns true if any setting changed (already
// saved + applied to jtSettings() on return)
bool jtRunSettingsDialog(void);

// ── error form (bottom-docked panel; ErrorForm.cs) ──────────────────────────
// show (or reload) the error panel for fname with these lints
void jtErrorFormShow(const std::string& fname, const std::vector<jsontools::JsonLint>& lints);
void jtErrorFormReload(const std::string& fname, const std::vector<jsontools::JsonLint>& lints);
void jtErrorFormHide(void);
bool jtErrorFormIsVisible(void);
void jtErrorFormApplyStyling(void);
void jtErrorFormShutdown(void);

// ── sort form (SortForm.cs; floating panel) ─────────────────────────────────
void jtShowSortForm(const std::string& pathText);
void jtSortFormApplyStyling(void);

// ── JSON-to-CSV dialog (JsonToCsvForm.cs; modal) ────────────────────────────
void jtShowJsonToCsvDialog(jsontools::JNodePtr json);

// ── regex search form (RegexSearchForm.cs; floating panel) ──────────────────
void jtShowRegexSearchForm(void);
void jtRegexSearchFormApplyStyling(void);

// ── find/replace form (FindReplaceForm.cs; floating panel owned by a tree) ──
void jtShowFindReplaceForm(JTTreeViewer* treeViewer);
void jtCloseFindReplaceFormFor(JTTreeViewer* treeViewer);
