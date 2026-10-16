// JsonTools.macos — TreeViewer.mm: see TreeViewer.h. Port of TreeViewer.cs;
// the WinForms TreeView becomes a lazily-populated NSOutlineView, laid out
// like the Windows panel: query box + button column on top, tree in the
// middle, Find/replace + current-path box at the bottom.

#import "TreeViewer.h"

#include "JsonTabularize.h"
#include "RemesPath.h"

#import "GrepperForm.h"
#import "JtForms.h"
#include "JtNpp.h"
#include "JtSettings.h"

#include <algorithm>

using namespace jsontools;
using namespace jtnpp;
using namespace jtcore;

static const size_t MAX_TREENODE_JSON_LENGTH = 1024;

// defined in PluginEntry.mm
extern int jtFuncItemCmdId(int funcItemIndex);
extern const int JT_TREE_FUNC_INDEX;

// ── tree items ───────────────────────────────────────────────────────────────
@interface JTTreeItem : NSObject {
  @public
    JNodePtr jnode;
}
@property(nonatomic, copy) NSString* displayText;
@property(nonatomic, copy) NSString* keyName; // object key; index string for arrays; "" root
@property(nonatomic) bool isArrayMember;
@property(nonatomic, weak) JTTreeItem* parent;
@property(nonatomic, strong) NSMutableArray<JTTreeItem*>* children; // nil until built
@end

@implementation JTTreeItem
@end

static NSString* JsonStringCappedLength(const JNodePtr& node) {
    std::string jsonstr = node->ToString();
    if (jsonstr.size() > MAX_TREENODE_JSON_LENGTH)
        jsonstr = jsonstr.substr(0, MAX_TREENODE_JSON_LENGTH) + "...";
    return [NSString stringWithUTF8String:jsonstr.c_str()] ?: @"";
}

static NSString* TextForTreeNode(const std::string& key, const JNodePtr& node) {
    std::string escapedKey = JNode::StrToString(key, false);
    std::string text;
    if (JArray* arr = AsArr(node)) {
        text = arr->Length() == 0 ? escapedKey + " : []"
                                  : escapedKey + " : [" + std::to_string(arr->Length()) + "]";
    } else if (JObject* obj = AsObj(node)) {
        text = obj->Length() == 0 ? escapedKey + " : {}"
                                  : escapedKey + " : {" + std::to_string(obj->Length()) + "}";
    } else {
        return [NSString stringWithFormat:@"%s : %@", escapedKey.c_str(),
                                          JsonStringCappedLength(node)];
    }
    return [NSString stringWithUTF8String:text.c_str()] ?: @"";
}

static NSImage* TypeIcon(Dtype type) {
    static NSMutableDictionary<NSNumber*, NSImage*>* cache = [NSMutableDictionary dictionary];
    NSNumber* key = @((int)type);
    NSImage* cached = cache[key];
    if (cached) return cached;
    NSString* glyph;
    NSColor* color;
    switch (type) {
        case DT_ARR: glyph = @"[]"; color = NSColor.systemBlueColor; break;
        case DT_BOOL: glyph = @"b"; color = NSColor.systemGreenColor; break;
        case DT_FLOAT: glyph = @".1"; color = NSColor.systemRedColor; break;
        case DT_INT: glyph = @"1"; color = NSColor.systemRedColor; break;
        case DT_OBJ: glyph = @"{}"; color = NSColor.systemPurpleColor; break;
        case DT_STR: glyph = @"s"; color = NSColor.systemOrangeColor; break;
        default: glyph = @"∅"; color = NSColor.systemGrayColor; break;
    }
    NSImage* img = [NSImage imageWithSize:NSMakeSize(16, 16)
                                  flipped:NO
                           drawingHandler:^BOOL(NSRect rect) {
                             NSDictionary* attrs = @{
                                 NSFontAttributeName : [NSFont boldSystemFontOfSize:9],
                                 NSForegroundColorAttributeName : color,
                             };
                             NSSize sz = [glyph sizeWithAttributes:attrs];
                             [glyph drawAtPoint:NSMakePoint((16 - sz.width) / 2,
                                                            (16 - sz.height) / 2)
                                 withAttributes:attrs];
                             return YES;
                           }];
    [img setTemplate:NO];
    cache[key] = img;
    return img;
}

// ── the tree viewer ──────────────────────────────────────────────────────────
@interface JTTreeViewer () <NSOutlineViewDataSource, NSOutlineViewDelegate, NSTextViewDelegate> {
    std::string _fname;
    JNodePtr _json;
    JNodePtr _queryResult;
    RemesParser _remesParser;
    char _csvDelim;
    char _csvQuote;
}
@property(nonatomic, strong) NSView* containerView;
@property(nonatomic, strong) NSTextView* queryBox;
@property(nonatomic, strong) NSOutlineView* outline;
@property(nonatomic, strong) NSTextField* currentPathBox;
@property(nonatomic, strong) NSPopUpButton* documentTypeCombo;
@property(nonatomic, strong) JTTreeItem* rootItem;
@end

@implementation JTTreeViewer

- (instancetype)initWithJson:(JNodePtr)json {
    if (!(self = [super init])) return nil;
    _fname = GetCurrentFilePath();
    _json = json;
    _queryResult = json;
    _csvDelim = '\0';
    _csvQuote = '\0';
    _shouldRefresh = false;
    _documentTypeIndexChangeWasAutomatic = false;
    _panelHandle = 0;
    _isGrepperTree = false;
    [self buildView];
    self.documentTypeIndexChangeWasAutomatic = true;
    [self setDocumentTypeComboBoxIndex:[self currentDocumentType]];
    self.documentTypeIndexChangeWasAutomatic = false;
    return self;
}

- (std::string)fname { return _fname; }
- (void)setFname:(const std::string&)fname { _fname = fname; }
- (JNodePtr)json { return _json; }
- (void)setJson:(JNodePtr)json { _json = json; }
- (JNodePtr)queryResult { return _queryResult; }
- (NSView*)view { return _containerView; }

- (DocumentType)currentDocumentType {
    JsonFileInfo* info = nullptr;
    if (TryGetInfoForFile(_fname, &info)) return info->documentType;
    return DocumentType::JSON;
}

- (bool)usesSelections {
    JsonFileInfo* info = nullptr;
    if (TryGetInfoForFile(_fname, &info)) return info->usesSelections;
    return false;
}

// ── view construction ────────────────────────────────────────────────────────
- (NSButton*)makeButton:(NSString*)title action:(SEL)action {
    NSButton* b = [NSButton buttonWithTitle:title target:self action:action];
    b.bezelStyle = NSBezelStyleRounded;
    b.controlSize = NSControlSizeSmall;
    b.font = [NSFont systemFontOfSize:[NSFont smallSystemFontSize]];
    b.translatesAutoresizingMaskIntoConstraints = NO;
    return b;
}

- (void)buildView {
    NSView* root = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 360, 600)];

    // query box (multi-line) in a scroll view
    NSScrollView* querySV = [[NSScrollView alloc] init];
    querySV.translatesAutoresizingMaskIntoConstraints = NO;
    querySV.hasVerticalScroller = YES;
    querySV.borderType = NSBezelBorder;
    NSTextView* query = [[NSTextView alloc] init];
    query.font = [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular];
    query.richText = NO;
    query.automaticQuoteSubstitutionEnabled = NO;
    query.automaticDashSubstitutionEnabled = NO;
    query.automaticTextReplacementEnabled = NO;
    query.allowsUndo = YES;
    query.string = @"@";
    query.delegate = self;
    query.minSize = NSMakeSize(0, 0);
    query.maxSize = NSMakeSize(FLT_MAX, FLT_MAX);
    query.verticallyResizable = YES;
    query.horizontallyResizable = NO;
    query.autoresizingMask = NSViewWidthSizable;
    querySV.documentView = query;
    _queryBox = query;
    [root addSubview:querySV];

    NSButton* submitBtn = [self makeButton:@"Submit query" action:@selector(submitQueryClicked:)];
    NSButton* toCsvBtn = [self makeButton:@"Query to CSV" action:@selector(queryToCsvClicked:)];
    NSButton* saveBtn = [self makeButton:@"Save query result" action:@selector(saveQueryResultClicked:)];
    NSButton* refreshBtn = [self makeButton:@"Refresh" action:@selector(refreshClicked:)];
    [root addSubview:submitBtn];
    [root addSubview:toCsvBtn];
    [root addSubview:saveBtn];
    [root addSubview:refreshBtn];

    NSPopUpButton* combo = [[NSPopUpButton alloc] init];
    combo.translatesAutoresizingMaskIntoConstraints = NO;
    combo.controlSize = NSControlSizeSmall;
    combo.font = [NSFont systemFontOfSize:[NSFont smallSystemFontSize]];
    [combo addItemsWithTitles:@[ @"JSON mode", @"JSON Lines mode", @"INI mode", @"Regex mode" ]];
    combo.target = self;
    combo.action = @selector(documentTypeComboChanged:);
    _documentTypeCombo = combo;
    [root addSubview:combo];

    // outline view
    NSScrollView* treeSV = [[NSScrollView alloc] init];
    treeSV.translatesAutoresizingMaskIntoConstraints = NO;
    treeSV.hasVerticalScroller = YES;
    treeSV.hasHorizontalScroller = YES;
    treeSV.borderType = NSBezelBorder;
    NSOutlineView* outline = [[NSOutlineView alloc] init];
    NSTableColumn* col = [[NSTableColumn alloc] initWithIdentifier:@"json"];
    col.resizingMask = NSTableColumnAutoresizingMask;
    [outline addTableColumn:col];
    outline.outlineTableColumn = col;
    outline.headerView = nil;
    outline.dataSource = self;
    outline.delegate = self;
    outline.allowsMultipleSelection = NO;
    outline.autoresizesOutlineColumn = YES;
    outline.columnAutoresizingStyle = NSTableViewFirstColumnOnlyAutoresizingStyle;
    outline.menu = [self buildContextMenu];
    treeSV.documentView = outline;
    _outline = outline;
    [root addSubview:treeSV];

    NSButton* findReplaceBtn = [self makeButton:@"Find/replace" action:@selector(findReplaceClicked:)];
    [root addSubview:findReplaceBtn];
    NSTextField* pathBox = [[NSTextField alloc] init];
    pathBox.translatesAutoresizingMaskIntoConstraints = NO;
    pathBox.editable = NO;
    pathBox.selectable = YES;
    pathBox.bezeled = YES;
    pathBox.font = [NSFont monospacedSystemFontOfSize:[NSFont smallSystemFontSize]
                                               weight:NSFontWeightRegular];
    _currentPathBox = pathBox;
    [root addSubview:pathBox];

    CGFloat pad = 6;
    [NSLayoutConstraint activateConstraints:@[
        [querySV.topAnchor constraintEqualToAnchor:root.topAnchor constant:pad],
        [querySV.leadingAnchor constraintEqualToAnchor:root.leadingAnchor constant:pad],
        [querySV.trailingAnchor constraintEqualToAnchor:submitBtn.leadingAnchor constant:-pad],
        [querySV.heightAnchor constraintEqualToConstant:112],

        [submitBtn.topAnchor constraintEqualToAnchor:root.topAnchor constant:pad],
        [submitBtn.trailingAnchor constraintEqualToAnchor:root.trailingAnchor constant:-pad],
        [submitBtn.widthAnchor constraintEqualToConstant:130],
        [toCsvBtn.topAnchor constraintEqualToAnchor:submitBtn.bottomAnchor constant:2],
        [toCsvBtn.trailingAnchor constraintEqualToAnchor:submitBtn.trailingAnchor],
        [toCsvBtn.widthAnchor constraintEqualToAnchor:submitBtn.widthAnchor],
        [saveBtn.topAnchor constraintEqualToAnchor:toCsvBtn.bottomAnchor constant:2],
        [saveBtn.trailingAnchor constraintEqualToAnchor:submitBtn.trailingAnchor],
        [saveBtn.widthAnchor constraintEqualToAnchor:submitBtn.widthAnchor],
        [refreshBtn.topAnchor constraintEqualToAnchor:saveBtn.bottomAnchor constant:2],
        [refreshBtn.trailingAnchor constraintEqualToAnchor:submitBtn.trailingAnchor],
        [refreshBtn.widthAnchor constraintEqualToAnchor:submitBtn.widthAnchor],
        [combo.topAnchor constraintEqualToAnchor:refreshBtn.bottomAnchor constant:2],
        [combo.trailingAnchor constraintEqualToAnchor:submitBtn.trailingAnchor],
        [combo.widthAnchor constraintEqualToAnchor:submitBtn.widthAnchor],

        [treeSV.topAnchor constraintEqualToAnchor:querySV.bottomAnchor constant:pad],
        [treeSV.topAnchor constraintGreaterThanOrEqualToAnchor:combo.bottomAnchor constant:pad],
        [treeSV.leadingAnchor constraintEqualToAnchor:root.leadingAnchor constant:pad],
        [treeSV.trailingAnchor constraintEqualToAnchor:root.trailingAnchor constant:-pad],
        [treeSV.bottomAnchor constraintEqualToAnchor:findReplaceBtn.topAnchor constant:-pad],

        [findReplaceBtn.leadingAnchor constraintEqualToAnchor:root.leadingAnchor constant:pad],
        [findReplaceBtn.bottomAnchor constraintEqualToAnchor:root.bottomAnchor constant:-pad],
        [pathBox.leadingAnchor constraintEqualToAnchor:findReplaceBtn.trailingAnchor constant:pad],
        [pathBox.trailingAnchor constraintEqualToAnchor:root.trailingAnchor constant:-pad],
        [pathBox.centerYAnchor constraintEqualToAnchor:findReplaceBtn.centerYAnchor],
    ]];
    _containerView = root;
    [self applyStyling];
}

- (NSMenu*)buildContextMenu {
    NSMenu* menu = [[NSMenu alloc] initWithTitle:@"Node"];
    [menu addItemWithTitle:@"Value to clipboard" action:@selector(ctxValueToClipboard:) keyEquivalent:@""];
    NSMenuItem* keyItem = [menu addItemWithTitle:@"Key/index to clipboard" action:nil keyEquivalent:@""];
    NSMenu* keyMenu = [[NSMenu alloc] init];
    [keyMenu addItemWithTitle:@"JavaScript style" action:@selector(ctxKeyJs:) keyEquivalent:@""];
    [keyMenu addItemWithTitle:@"Python style" action:@selector(ctxKeyPy:) keyEquivalent:@""];
    [keyMenu addItemWithTitle:@"RemesPath style" action:@selector(ctxKeyRemes:) keyEquivalent:@""];
    [keyMenu addItemWithTitle:@"Use path_separator setting" action:@selector(ctxKeySep:) keyEquivalent:@""];
    keyItem.submenu = keyMenu;
    NSMenuItem* pathItem = [menu addItemWithTitle:@"Path to clipboard" action:nil keyEquivalent:@""];
    NSMenu* pathMenu = [[NSMenu alloc] init];
    [pathMenu addItemWithTitle:@"JavaScript style" action:@selector(ctxPathJs:) keyEquivalent:@""];
    [pathMenu addItemWithTitle:@"Python style" action:@selector(ctxPathPy:) keyEquivalent:@""];
    [pathMenu addItemWithTitle:@"RemesPath style" action:@selector(ctxPathRemes:) keyEquivalent:@""];
    [pathMenu addItemWithTitle:@"Use path_separator setting" action:@selector(ctxPathSep:) keyEquivalent:@""];
    pathItem.submenu = pathMenu;
    [menu addItemWithTitle:@"Expand/collapse all subtrees" action:@selector(ctxToggleSubtrees:) keyEquivalent:@""];
    [menu addItemWithTitle:@"Select this" action:@selector(ctxSelectThis:) keyEquivalent:@""];
    [menu addItemWithTitle:@"Open sort form" action:@selector(ctxOpenSortForm:) keyEquivalent:@""];
    [menu addItemWithTitle:@"Select all children" action:@selector(ctxSelectAllChildren:) keyEquivalent:@""];
    [menu addItemWithTitle:@"Make this node's position visible" action:@selector(ctxMakeVisible:) keyEquivalent:@""];
    for (NSMenuItem* item in menu.itemArray) item.target = self;
    for (NSMenuItem* item in keyMenu.itemArray) item.target = self;
    for (NSMenuItem* item in pathMenu.itemArray) item.target = self;
    return menu;
}

- (void)applyStyling {
    CGFloat fontSize = jtSettings().tree_view_font_size;
    if (fontSize < 6) fontSize = 11;
    _outline.font = [NSFont monospacedSystemFontOfSize:fontSize weight:NSFontWeightRegular];
    _outline.rowHeight = fontSize + 7;
    [_outline reloadData];
}

- (void)changeTreeViewFont { [self applyStyling]; }

// ── panel registration / visibility ──────────────────────────────────────────
- (void)registerPanelIfNeeded:(const std::string&)title {
    if (_panelHandle != 0) return;
    tTbData tb = {};
    tb.hClient = (__bridge void*)_containerView;
    tb.pszName = title.c_str();
    tb.dlgID = JT_TREE_FUNC_INDEX;
    tb.uMask = DWS_DF_CONT_RIGHT;
    tb.pszModuleName = "JsonTools";
    intptr_t h = Npp(NPPM_DMMREGASDCKDLG, 0, (intptr_t)&tb);
    if (h > 0) _panelHandle = (uint64_t)h;
}

- (bool)isVisibleOnScreen {
    return _containerView && _containerView.window != nil &&
           !_containerView.hiddenOrHasHiddenAncestor;
}

- (void)showPanel {
    if (_panelHandle == 0) [self registerPanelIfNeeded:[self panelTitle]];
    if (_panelHandle == 0) return;
    Npp(NPPM_DMM_SHOWPANEL, (uintptr_t)_panelHandle, 0);
    if (!_isGrepperTree)
        Npp(NPPM_SETMENUITEMCHECK, (uintptr_t)jtFuncItemCmdId(JT_TREE_FUNC_INDEX), 1);
}

- (void)hidePanel {
    if (_panelHandle == 0) return;
    Npp(NPPM_DMM_HIDEPANEL, (uintptr_t)_panelHandle, 0);
    jtCloseFindReplaceFormFor(self);
    if (!_isGrepperTree)
        Npp(NPPM_SETMENUITEMCHECK, (uintptr_t)jtFuncItemCmdId(JT_TREE_FUNC_INDEX), 0);
}

- (void)closePanel {
    jtCloseFindReplaceFormFor(self);
    if (_panelHandle != 0) {
        Npp(NPPM_DMM_UNREGISTERPANEL, (uintptr_t)_panelHandle, 0);
        _panelHandle = 0;
    }
    if (!_isGrepperTree)
        Npp(NPPM_SETMENUITEMCHECK, (uintptr_t)jtFuncItemCmdId(JT_TREE_FUNC_INDEX), 0);
}

- (std::string)panelTitle {
    if (_isGrepperTree) return "JSON from files and APIs tree";
    size_t slash = _fname.rfind('/');
    std::string base = slash == std::string::npos ? _fname : _fname.substr(slash + 1);
    return "Json Tree View for " + base;
}

- (void)renameTo:(const std::string&)newFname isFilename:(bool)isFilename {
    if (isFilename) _fname = newFname;
    // the host keeps the registration-time title; new registrations pick up the
    // new name (NPPM_DMMUPDATEDISPINFO is a no-op on this host)
}

- (void)focusQueryBox {
    [_queryBox.window makeFirstResponder:_queryBox];
}

- (void)setQueryBoxText:(NSString*)text { _queryBox.string = text ?: @""; }

- (void)expandRootNode {
    if (_rootItem) [_outline expandItem:_rootItem];
}

// ── populating the tree ──────────────────────────────────────────────────────
- (int64_t)intervalForJson:(const JNodePtr&)json {
    int64_t interval = 0;
    int64_t maxLen = jtSettings().max_json_length_full_tree;
    if (maxLen < 1) maxLen = 1;
    if (JArray* arr = AsArr(json)) interval = (int64_t)arr->Length() / maxLen;
    else if (JObject* obj = AsObj(json)) interval = (int64_t)obj->Length() / maxLen;
    return interval < 1 ? 1 : interval;
}

- (NSMutableArray<JTTreeItem*>*)buildChildrenOf:(JTTreeItem*)item {
    NSMutableArray<JTTreeItem*>* out = [NSMutableArray array];
    JNodePtr json = item->jnode;
    int64_t interval = [self intervalForJson:json];
    if (JArray* arr = AsArr(json)) {
        for (size_t ii = 0; ii < arr->children.size(); ii += (size_t)interval) {
            JNodePtr child = arr->children[ii];
            JTTreeItem* childItem = [[JTTreeItem alloc] init];
            childItem->jnode = child;
            childItem.keyName = [NSString stringWithFormat:@"%zu", ii];
            childItem.isArrayMember = true;
            childItem.displayText = TextForTreeNode(std::to_string(ii), child);
            childItem.parent = item;
            [out addObject:childItem];
        }
    } else if (JObject* obj = AsObj(json)) {
        // root of a selection-based doc: order keys by selection start
        std::vector<std::string> keys;
        for (auto& kv : obj->children) keys.push_back(kv.first);
        if (item.parent == nil && [self usesSelections]) {
            bool allStartEnd = true;
            for (auto& k : keys)
                if (!IsStartEnd(k)) allStartEnd = false;
            if (allStartEnd)
                std::sort(keys.begin(), keys.end(), [](const std::string& a, const std::string& b) {
                    return StartEndCompareByStart(a, b) < 0;
                });
        }
        int64_t count = 0;
        for (const std::string& key : keys) {
            if (count++ % interval != 0) continue;
            JNodePtr child;
            obj->TryGetValue(key, child);
            if (!child) continue;
            JTTreeItem* childItem = [[JTTreeItem alloc] init];
            childItem->jnode = child;
            childItem.keyName = [NSString stringWithUTF8String:key.c_str()] ?: @"";
            childItem.isArrayMember = false;
            childItem.displayText = TextForTreeNode(key, child);
            childItem.parent = item;
            [out addObject:childItem];
        }
    }
    return out;
}

- (void)jsonTreePopulate:(JNodePtr)json {
    if (!json) {
        MsgBoxWarning("Can't populate JSON tree",
                      "Cannot populate the JSON tree because no JSON is stored.");
        return;
    }
    _json = json;
    JTTreeItem* root = [[JTTreeItem alloc] init];
    root->jnode = json;
    root.keyName = @"";
    root.isArrayMember = false;
    if (AsArr(json) || AsObj(json)) {
        root.displayText = TextForTreeNode("JSON", json);
    } else if (json->IsStrValue() && json->GetStr().size() > MAX_TREENODE_JSON_LENGTH) {
        std::string s = json->GetStr().substr(0, MAX_TREENODE_JSON_LENGTH);
        // avoid splitting a UTF-8 sequence at the cap
        while (!s.empty() && ((unsigned char)s.back() & 0xC0) == 0x80) s.pop_back();
        root.displayText =
            [NSString stringWithFormat:@"%s...", JNode::StrToString(s, true).c_str()];
    } else {
        root.displayText = JsonStringCappedLength(json);
    }
    _rootItem = root;
    [_outline reloadData];
    [_outline expandItem:root];
}

// ── NSOutlineView datasource/delegate ────────────────────────────────────────
- (NSInteger)outlineView:(NSOutlineView*)ov numberOfChildrenOfItem:(id)item {
    if (item == nil) return _rootItem ? 1 : 0;
    JTTreeItem* ti = (JTTreeItem*)item;
    if (!ti.children) ti.children = [self buildChildrenOf:ti];
    return (NSInteger)ti.children.count;
}

- (id)outlineView:(NSOutlineView*)ov child:(NSInteger)index ofItem:(id)item {
    if (item == nil) return _rootItem;
    JTTreeItem* ti = (JTTreeItem*)item;
    if (!ti.children) ti.children = [self buildChildrenOf:ti];
    return ti.children[(NSUInteger)index];
}

- (BOOL)outlineView:(NSOutlineView*)ov isItemExpandable:(id)item {
    JTTreeItem* ti = (JTTreeItem*)item;
    if (JArray* arr = AsArr(ti->jnode)) return arr->Length() > 0;
    if (JObject* obj = AsObj(ti->jnode)) return obj->Length() > 0;
    return NO;
}

- (NSView*)outlineView:(NSOutlineView*)ov viewForTableColumn:(NSTableColumn*)col item:(id)item {
    JTTreeItem* ti = (JTTreeItem*)item;
    NSTableCellView* cell = [ov makeViewWithIdentifier:@"jtcell" owner:self];
    if (!cell) {
        cell = [[NSTableCellView alloc] init];
        cell.identifier = @"jtcell";
        NSImageView* iv = [[NSImageView alloc] init];
        iv.translatesAutoresizingMaskIntoConstraints = NO;
        [cell addSubview:iv];
        cell.imageView = iv;
        NSTextField* tf = [NSTextField labelWithString:@""];
        tf.translatesAutoresizingMaskIntoConstraints = NO;
        tf.lineBreakMode = NSLineBreakByTruncatingTail;
        [cell addSubview:tf];
        cell.textField = tf;
        [NSLayoutConstraint activateConstraints:@[
            [iv.leadingAnchor constraintEqualToAnchor:cell.leadingAnchor],
            [iv.centerYAnchor constraintEqualToAnchor:cell.centerYAnchor],
            [iv.widthAnchor constraintEqualToConstant:16],
            [iv.heightAnchor constraintEqualToConstant:16],
            [tf.leadingAnchor constraintEqualToAnchor:iv.trailingAnchor constant:2],
            [tf.trailingAnchor constraintEqualToAnchor:cell.trailingAnchor],
            [tf.centerYAnchor constraintEqualToAnchor:cell.centerYAnchor],
        ]];
    }
    cell.textField.stringValue = ti.displayText ?: @"";
    cell.textField.font = _outline.font;
    bool showImages = jtSettings().tree_node_images;
    cell.imageView.hidden = !showImages;
    if (showImages) cell.imageView.image = TypeIcon(ti->jnode->type);
    return cell;
}

- (void)outlineViewSelectionDidChange:(NSNotification*)note {
    [self handleNodeSelection];
}

- (void)handleNodeSelection {
    if (GetCurrentFilePath() != _fname) return;
    JTTreeItem* item = [self selectedItem];
    if (!item) return;
    GotoPos([self nodePosInJsonDoc:item]);
    _currentPathBox.stringValue =
        [NSString stringWithUTF8String:[self pathToTreeItem:item
                                                      style:jtSettings().key_style
                                                  separator:JNode::DEFAULT_PATH_SEPARATOR]
                                            .c_str()] ?: @"";
}

- (JTTreeItem*)selectedItem {
    NSInteger row = _outline.selectedRow;
    if (row < 0) {
        row = _outline.clickedRow;
        if (row < 0) return nil;
    }
    return [_outline itemAtRow:row];
}

// ── paths / keys of tree items ───────────────────────────────────────────────
- (std::string)keyOfTreeItem:(JTTreeItem*)item style:(KeyStyle)style separator:(char)separator {
    if (item.isArrayMember) {
        int64_t idx = strtoll(item.keyName.UTF8String ?: "0", nullptr, 10);
        return JNode::FormatIndex(idx, separator);
    }
    try {
        return JNode::FormatKey(item.keyName.UTF8String ?: "", style, separator);
    } catch (const std::exception& ex) {
        MsgBoxError("Error while formatting key of tree node", ex.what());
        return "";
    }
}

- (std::string)pathToTreeItem:(JTTreeItem*)item style:(KeyStyle)style separator:(char)separator {
    std::string path;
    while (item && item.parent) {
        path = [self keyOfTreeItem:item style:style separator:separator] + path;
        item = item.parent;
    }
    return path;
}

// for selection-based docs, the enclosing "start,end" selection of the item
- (std::pair<int64_t, int64_t>)parentSelectionStartEnd:(JTTreeItem*)item {
    if (![self usesSelections]) return {0, -1};
    while (item && item.parent) {
        if (item.parent.parent == nil) {
            int64_t start, end;
            if (item.keyName && ParseStartEnd(item.keyName.UTF8String ?: "", start, end))
                return {start, end};
        }
        item = item.parent;
    }
    return {0, -1};
}

- (int64_t)nodePosInJsonDoc:(JTTreeItem*)item {
    return item->jnode->position + [self parentSelectionStartEnd:item].first;
}

// ── query submission (SubmitQueryButton_Click) ───────────────────────────────
- (void)submitQuery {
    if (!_json) return;
    if (_shouldRefresh && GetCurrentFilePath() == _fname) [self refreshClicked];
    bool usesSelections = [self usesSelections];
    std::string query = _queryBox.string.UTF8String ?: "";
    JNodePtr queryFunc;
    try {
        queryFunc = _remesParser.Compile(query);
    } catch (const std::exception& ex) {
        MsgBoxError("Compilation error in RemesPath query",
                    "Could not execute query " + query + " because of compilation error:\n" +
                        RemesParser::PrettifyException(ex));
        return;
    }
    JNodePtr treeFunc;
    auto runtimeError = [&query](const std::exception& ex, const std::string& selKey) {
        std::string expretty = RemesParser::PrettifyException(ex);
        if (!selKey.empty()) {
            MsgBoxError("Runtime error while executing query on selection",
                        "While executing query " + query + " on selection " + selKey +
                            ", encountered runtime error:\n" + expretty);
        } else {
            MsgBoxError("Runtime error while executing query",
                        "While executing query " + query + ", encountered runtime error:\n" +
                            expretty);
        }
    };
    if (queryFunc->IsMutator()) {
        bool isMultiStepQuery = dynamic_cast<JQueryContext*>(queryFunc.get()) != nullptr;
        if (usesSelections) {
            JObject* obj = AsObj(_json);
            auto queryObj = std::make_shared<JObject>();
            auto treeObj = isMultiStepQuery ? std::make_shared<JObject>() : queryObj;
            if (obj) {
                for (auto& [key, value] : obj->children) {
                    try {
                        JNodePtr sub = queryFunc->Operate(value);
                        if (isMultiStepQuery) {
                            treeObj->children.set(key, sub);
                            queryObj->children.set(key, value);
                        } else {
                            queryObj->children.set(key, sub);
                        }
                    } catch (const std::exception& ex) {
                        runtimeError(ex, key);
                    }
                }
            }
            treeFunc = treeObj;
            queryFunc = queryObj;
        } else {
            try {
                JNodePtr result = queryFunc->Operate(_json);
                treeFunc = result;
                queryFunc = isMultiStepQuery ? _json : result;
            } catch (const std::exception& ex) {
                runtimeError(ex, "");
                return;
            }
        }
        std::function<std::string(const JNodePtr&)> formatter = PrettyPrintFromSettings;
        DocumentType documentType = [self currentDocumentType];
        if (documentType == DocumentType::INI && AsObj(queryFunc)) {
            try {
                AsObj(queryFunc)->StringifyAllValuesInIniFile();
            } catch (const std::exception& ex) {
                MsgBoxError("Error while formatting ini file values after RemesPath mutation",
                            std::string("Could not mutate ini file because of error:\n") +
                                ex.what());
                return;
            }
        } else if (documentType == DocumentType::JSONL && AsArr(queryFunc)) {
            formatter = ToJsonLinesFromSettings;
        } else if (documentType == DocumentType::REGEX) {
            formatter = [](const JNodePtr& x) { return x->ValueOrToString(); };
        }
        auto keyChanges = ReformatFileWithJson(queryFunc, formatter, usesSelections);
        if (isMultiStepQuery && usesSelections && AsObj(treeFunc)) {
            JObject* treeObj = AsObj(treeFunc);
            std::map<std::string, std::pair<std::string, JNodePtr>> treeKeyChanges;
            for (auto& [changedKey, change] : keyChanges) {
                JNodePtr changedKeyVal;
                if (treeObj->TryGetValue(changedKey, changedKeyVal))
                    treeKeyChanges[changedKey] = {change.first, changedKeyVal};
            }
            RenameAll(treeKeyChanges, treeObj);
        }
        _json = queryFunc;
        JsonFileInfo* info = nullptr;
        if (TryGetInfoForFile(_fname, &info)) info->json = queryFunc;
        _queryResult = treeFunc;
    } else if (queryFunc->CanOperate()) {
        if (usesSelections) {
            JObject* obj = AsObj(_json);
            auto queryObj = std::make_shared<JObject>();
            if (obj) {
                for (auto& [key, value] : obj->children) {
                    try {
                        queryObj->children.set(key, queryFunc->Operate(value));
                    } catch (const std::exception& ex) {
                        runtimeError(ex, key);
                    }
                }
            }
            _queryResult = queryObj;
        } else {
            try {
                _queryResult = queryFunc->Operate(_json);
            } catch (const std::exception& ex) {
                runtimeError(ex, "");
                return;
            }
        }
        treeFunc = _queryResult;
    } else {
        _queryResult = queryFunc;
        treeFunc = _queryResult;
    }
    _csvDelim = csvDelimiterInLastQuery;
    _csvQuote = csvQuoteCharInLastQuery;
    JNodePtr docJson = _json; // the tree shows treeFunc, but _json stays the document JSON
    [self jsonTreePopulate:treeFunc];
    _json = docJson;
}

- (void)refreshClicked {
    _shouldRefresh = false;
    std::string curFname = GetCurrentFilePath();
    ParseResult pr = TryParseJson([self documentTypeFromComboBox]);
    if (!pr.node) return;
    [self transferToNewFile:curFname];
    _json = pr.node;
    _queryResult = pr.node;
    [self jsonTreePopulate:pr.node];
}

- (void)transferToNewFile:(const std::string&)newFname {
    std::string oldFname = _fname;
    if (oldFname != newFname && !_isGrepperTree) {
        [self renameTo:newFname isFilename:true];
        JsonFileInfo* info = nullptr;
        if (TryGetInfoForFile(newFname, &info)) info->tv = self;
        JsonFileInfo* oldInfo = nullptr;
        if (TryGetInfoForFile(oldFname, &oldInfo) && oldInfo->tv == self) oldInfo->tv = nil;
    }
}

// ── document type combo ──────────────────────────────────────────────────────
- (void)setDocumentTypeComboBoxIndex:(DocumentType)documentType {
    NSInteger idx = 0;
    switch (documentType) {
        case DocumentType::NONE:
        case DocumentType::JSON: idx = 0; break;
        case DocumentType::JSONL: idx = 1; break;
        case DocumentType::INI: idx = 2; break;
        case DocumentType::REGEX: idx = 3; break;
    }
    [_documentTypeCombo selectItemAtIndex:idx];
}

- (DocumentType)documentTypeFromComboBox {
    switch (_documentTypeCombo.indexOfSelectedItem) {
        case 0: return DocumentType::JSON;
        case 1: return DocumentType::JSONL;
        case 2: return DocumentType::INI;
        case 3: return DocumentType::REGEX;
        default: return DocumentType::NONE;
    }
}

- (void)documentTypeComboChanged:(id)sender {
    if (self.documentTypeIndexChangeWasAutomatic) return;
    DocumentType newDocumentType = [self documentTypeFromComboBox];
    DocumentType oldDocumentType = [self currentDocumentType];
    if (oldDocumentType == newDocumentType) return;
    ParseResult pr = TryParseJson(newDocumentType);
    [self transferToNewFile:GetCurrentFilePath()];
    if (pr.node) [self jsonTreePopulate:pr.node];
}

// ── button actions ───────────────────────────────────────────────────────────
- (void)submitQueryClicked:(id)sender { [self submitQuery]; }
- (void)refreshClicked:(id)sender { [self refreshClicked]; }

- (void)queryToCsvClicked:(id)sender {
    if (_queryResult) jtShowJsonToCsvDialog(_queryResult);
}

- (void)saveQueryResultClicked:(id)sender {
    if (_queryResult) PrettyPrintJsonInNewFile(_queryResult);
}

- (void)findReplaceClicked:(id)sender { jtShowFindReplaceForm(self); }

// Cmd+Enter in the query box submits the query
- (BOOL)textView:(NSTextView*)textView doCommandBySelector:(SEL)commandSelector {
    if (commandSelector == @selector(insertNewline:) &&
        (NSApp.currentEvent.modifierFlags & NSEventModifierFlagCommand)) {
        [self submitQuery];
        return YES;
    }
    if (commandSelector == @selector(cancelOperation:)) {
        GrabFocus();
        return YES;
    }
    return NO;
}

// ── context menu actions ─────────────────────────────────────────────────────
- (void)ctxValueToClipboard:(id)sender {
    JTTreeItem* item = [self selectedItem];
    if (!item || AsArr(item->jnode) || AsObj(item->jnode)) return;
    TryCopyToClipboard(item->jnode->ToString());
}

- (void)copyKeyWithStyle:(KeyStyle)style separator:(char)separator {
    JTTreeItem* item = [self selectedItem];
    if (!item) return;
    TryCopyToClipboard([self keyOfTreeItem:item style:style separator:separator]);
}

- (void)copyPathWithStyle:(KeyStyle)style separator:(char)separator {
    JTTreeItem* item = [self selectedItem];
    if (!item) return;
    TryCopyToClipboard([self pathToTreeItem:item style:style separator:separator]);
}

- (void)ctxKeyJs:(id)sender { [self copyKeyWithStyle:KeyStyle::JavaScript separator:JNode::DEFAULT_PATH_SEPARATOR]; }
- (void)ctxKeyPy:(id)sender { [self copyKeyWithStyle:KeyStyle::Python separator:JNode::DEFAULT_PATH_SEPARATOR]; }
- (void)ctxKeyRemes:(id)sender { [self copyKeyWithStyle:KeyStyle::RemesPath separator:JNode::DEFAULT_PATH_SEPARATOR]; }
- (void)ctxKeySep:(id)sender { [self copyKeyWithStyle:jtSettings().key_style separator:pathSeparator]; }
- (void)ctxPathJs:(id)sender { [self copyPathWithStyle:KeyStyle::JavaScript separator:JNode::DEFAULT_PATH_SEPARATOR]; }
- (void)ctxPathPy:(id)sender { [self copyPathWithStyle:KeyStyle::Python separator:JNode::DEFAULT_PATH_SEPARATOR]; }
- (void)ctxPathRemes:(id)sender { [self copyPathWithStyle:KeyStyle::RemesPath separator:JNode::DEFAULT_PATH_SEPARATOR]; }
- (void)ctxPathSep:(id)sender { [self copyPathWithStyle:jtSettings().key_style separator:pathSeparator]; }

- (void)ctxToggleSubtrees:(id)sender {
    JTTreeItem* item = [self selectedItem];
    if (!item) return;
    if ([_outline isItemExpanded:item]) {
        [_outline collapseItem:item collapseChildren:YES];
    } else {
        // NSOutlineView expandItem:expandChildren: builds lazily via the datasource
        [_outline expandItem:item expandChildren:YES];
    }
}

- (void)ctxSelectThis:(id)sender {
    JTTreeItem* item = [self selectedItem];
    if (!item) return;
    [self selectTreeItemJson:item];
}

- (void)ctxOpenSortForm:(id)sender {
    JTTreeItem* item = [self selectedItem];
    if (!item || (!AsArr(item->jnode) && !AsObj(item->jnode))) return;
    jtShowSortForm([self pathToTreeItem:item
                                  style:KeyStyle::RemesPath
                              separator:JNode::DEFAULT_PATH_SEPARATOR]);
}

- (void)ctxSelectAllChildren:(id)sender {
    JTTreeItem* item = [self selectedItem];
    if (!item) return;
    [self selectTreeItemJsonChildren:item];
}

- (void)ctxMakeVisible:(id)sender {
    JTTreeItem* item = [self selectedItem];
    if (!item) return;
    int64_t posOfNode = [self nodePosInJsonDoc:item];
    int64_t lineOfNode = LineFromPosition(posOfNode);
    EnsureLineVisible(lineOfNode);
    GotoPos(posOfNode);
}

// ── selecting document text for nodes ────────────────────────────────────────
// upstream TreeViewer.LengthOfStringInRegexMode (regex-mode nodes map to raw
// text, not JSON): figure out the UTF-8 length of each node's document text
- (bool)lengthOfStringInRegexMode:(const std::vector<JNodePtr>&)nodes
                      utf8Lengths:(std::vector<int64_t>&)utf8Lengths
                   selectionStart:(int64_t)selectionStart
                     selectionEnd:(int64_t)selectionEnd {
    static Regex numRegex(
        "(?:[+-]?(?:0x[\\da-fA-F]+|\\d+(?:\\.\\d*)?|\\.\\d+)(?:[eE][+-]?\\d+)?)");
    utf8Lengths.assign(nodes.size(), 0);
    std::string documentText;
    bool haveText = false;
    for (size_t ii = 0; ii < nodes.size(); ii++) {
        const JNodePtr& jnode = nodes[ii];
        if (AsArr(jnode) || AsObj(jnode)) {
            MsgBoxError("Can't select object or array in non-JSON",
                        "Cannot select an object or an array in a non-JSON document, as it does "
                        "not correspond to a specific text region");
            return false;
        }
        if (jnode->type == DT_INT || jnode->type == DT_FLOAT) {
            double d = jnode->type == DT_INT ? (double)jnode->GetInt() : jnode->GetDouble();
            int64_t nodepos = jnode->position;
            if (!haveText) {
                documentText = selectionEnd < 0 ? GetText() : GetSlice(selectionStart, selectionEnd);
                haveText = true;
            }
            Regex::Match m = numRegex.FirstMatch(documentText, nodepos);
            bool used = false;
            if (m.success && m.start == nodepos) {
                try {
                    double parsedNum = strtod(m.value.c_str(), nullptr);
                    if (parsedNum == d) {
                        utf8Lengths[ii] = m.length;
                        used = true;
                    }
                } catch (...) {
                }
            }
            if (!used) utf8Lengths[ii] = (int64_t)jnode->ToString().size();
        } else {
            std::string sb;
            JsonTabularizer::CsvStringToSb(sb, jnode, _csvDelim, _csvQuote, false);
            int64_t utf8Len = (int64_t)sb.size();
            if (sb.empty() || sb[0] != _csvQuote) {
                // the string may be quoted in the document even if it needn't be
                int64_t nodeStart = selectionStart + jnode->position;
                std::string firstChar = GetSlice(nodeStart, nodeStart + 1);
                if (!firstChar.empty() && firstChar[0] == _csvQuote) utf8Len += 2;
            }
            utf8Lengths[ii] = utf8Len;
        }
    }
    return true;
}

- (void)selectTreeItemJson:(JTTreeItem*)item {
    if (activeFname != _fname) return;
    int64_t len = DocLength();
    bool isRegex = [self currentDocumentType] == DocumentType::REGEX;
    auto [selectionStart, selectionEnd] = [self parentSelectionStartEnd:item];
    int64_t nodeStartPos = selectionStart + item->jnode->position;
    int64_t nodeEndPos = nodeStartPos;
    if (isRegex) {
        std::vector<int64_t> utf8Lengths;
        if (![self lengthOfStringInRegexMode:{item->jnode}
                                 utf8Lengths:utf8Lengths
                              selectionStart:selectionStart
                                selectionEnd:selectionEnd])
            return;
        nodeEndPos = nodeStartPos + utf8Lengths[0];
    } else {
        nodeEndPos = EndOfJNodeAtPos(nodeStartPos, selectionEnd < 0 ? len : selectionEnd);
    }
    if (!isRegex && nodeStartPos == nodeEndPos) {
        MsgBoxError("Couldn't select associated JSON",
                    "The selected tree node does not appear to correspond to a JSON element in "
                    "the document.");
    } else {
        ClearSelections();
        SetSelection(nodeStartPos, nodeEndPos);
        GrabFocus();
    }
}

- (void)selectTreeItemJsonChildren:(JTTreeItem*)item {
    if (activeFname != _fname) return;
    JsonFileInfo* info = nullptr;
    if (!TryGetInfoForFile(_fname, &info)) return;
    if (info->usesSelections && item.parent == nil && AsObj(_json)) {
        std::vector<std::string> keys;
        for (auto& kv : AsObj(_json)->children) keys.push_back(kv.first);
        SetSelectionsFromStartEnds(keys);
        return;
    }
    JNodePtr jnode = item->jnode;
    auto [selectionStartPos, selectionEndPos] = [self parentSelectionStartEnd:item];
    if (info->documentType == DocumentType::REGEX) {
        std::vector<JNodePtr> children;
        if (JArray* arr = AsArr(jnode)) {
            children = arr->children;
        } else if (JObject* obj = AsObj(jnode)) {
            for (auto& kv : obj->children) children.push_back(kv.second);
        }
        std::sort(children.begin(), children.end(),
                  [](const JNodePtr& a, const JNodePtr& b) { return a->position < b->position; });
        std::vector<int64_t> utf8Lengths;
        if ([self lengthOfStringInRegexMode:children
                                utf8Lengths:utf8Lengths
                             selectionStart:selectionStartPos
                               selectionEnd:selectionEndPos]) {
            ClearSelections();
            bool firstSelectionSet = false;
            for (size_t ii = 0; ii < utf8Lengths.size(); ii++) {
                int64_t startPos = children[ii]->position + selectionStartPos;
                int64_t endPos = startPos + utf8Lengths[ii];
                if (firstSelectionSet)
                    AddSelection(startPos, endPos);
                else
                    SetSelection(startPos, endPos);
                firstSelectionSet = true;
            }
        }
        return;
    }
    std::vector<int64_t> positions;
    if (JArray* arr = AsArr(jnode)) {
        for (JNodePtr& child : arr->children) positions.push_back(selectionStartPos + child->position);
    } else if (JObject* obj = AsObj(jnode)) {
        for (auto& kv : obj->children) positions.push_back(selectionStartPos + kv.second->position);
    } else {
        MsgBoxError("Can only select children of object or array",
                    "The selected JSON is not an object or array, and thus has no children.");
        return;
    }
    SelectAllChildren(positions, info->documentType == DocumentType::JSONL);
}

@end
