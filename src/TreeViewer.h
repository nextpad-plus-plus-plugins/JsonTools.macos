// JsonTools.macos — TreeViewer.h: port of Forms/TreeViewer.cs as a docked
// side panel. One instance per file (like Windows), each registered with the
// host docking API; the grepper form owns its own instance.

#pragma once

#import <Cocoa/Cocoa.h>

#include <string>

#include "JNode.h"
#include "JtCore.h"

@interface JTTreeViewer : NSObject

// the file this tree belongs to
- (std::string)fname;
- (void)setFname:(const std::string&)fname;

- (jsontools::JNodePtr)json;
- (void)setJson:(jsontools::JNodePtr)json; // does NOT repopulate

- (jsontools::JNodePtr)queryResult;

@property(nonatomic) bool shouldRefresh;
@property(nonatomic) bool documentTypeIndexChangeWasAutomatic;
@property(nonatomic) uint64_t panelHandle; // host docking handle (0 = unregistered)
@property(nonatomic) bool isGrepperTree;

- (instancetype)initWithJson:(jsontools::JNodePtr)json;
- (NSView*)view;
- (bool)isVisibleOnScreen;
- (void)showPanel;
- (void)hidePanel;
- (void)closePanel; // hide + unregister + release subforms

- (void)jsonTreePopulate:(jsontools::JNodePtr)json;
- (void)setDocumentTypeComboBoxIndex:(jsontools::DocumentType)documentType;
- (jsontools::DocumentType)documentTypeFromComboBox;
- (void)renameTo:(const std::string&)newFname isFilename:(bool)isFilename;
- (void)focusQueryBox;
- (void)setQueryBoxText:(NSString*)text;
- (void)submitQuery;      // SubmitQueryButton.PerformClick
- (void)refreshClicked;   // RefreshButton.PerformClick
- (void)expandRootNode;
- (void)applyStyling;     // colors + font from settings

@end
