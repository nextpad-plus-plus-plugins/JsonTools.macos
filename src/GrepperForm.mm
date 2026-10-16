// JsonTools.macos — GrepperForm.mm: see GrepperForm.h. Port of GrepperForm.cs.
// Grep/API-fetch run on a background queue with a progress sheet; results feed
// a dedicated TreeViewer panel, exactly like the Windows plugin.

#import "GrepperForm.h"

#include "RemesPath.h"

#include "JtCore.h"
#include "JtNpp.h"
#include "JtSettings.h"
#import "TreeViewer.h"

#include <fstream>
#include <set>
#include <sstream>

using namespace jsontools;
using namespace jtnpp;
using namespace jtcore;

static NSString* U8g(const std::string& s) {
    return [NSString stringWithUTF8String:s.c_str()] ?: @"";
}

static const int CHECKPOINT_COUNT = 25;

@interface JTGrepperForm () <NSWindowDelegate> {
    jsontools::JsonGrepper* _grepper;
    std::set<std::string> _filesFound;
    std::vector<std::string> _urlsQueried;
    std::string _directoriesVisitedFile;
    std::string _urlsQueriedFile;
}
@property(nonatomic, strong) NSWindow* window;
@property(nonatomic, strong) NSTextView* urlsBox;
@property(nonatomic, strong) NSComboBox* directoriesBox;
@property(nonatomic, strong) NSTextView* searchPatternsBox;
@property(nonatomic, strong) NSButton* recursiveCheck;
@property(nonatomic, strong) NSTableView* filesFoundTable;
@property(nonatomic, strong) NSMutableArray<NSString*>* filesFoundList;
@property(nonatomic, strong) NSPanel* progressPanel;
@property(nonatomic, strong) NSProgressIndicator* progressBar;
@property(nonatomic, strong) NSTextField* progressLabel;
@property(nonatomic, strong) JTTreeViewer* tv;
@property(nonatomic) bool busy;
@end

static JTGrepperForm* sGrepperForm = nil;

@implementation JTGrepperForm

+ (JTGrepperForm*)shared {
    return sGrepperForm;
}

+ (void)showOrFocus {
    if (sGrepperForm) {
        [sGrepperForm.window makeKeyAndOrderFront:nil];
        return;
    }
    sGrepperForm = [[JTGrepperForm alloc] init];
    [sGrepperForm.window makeKeyAndOrderFront:nil];
}

- (jsontools::JsonGrepper*)grepper {
    return _grepper;
}

- (instancetype)init {
    if (!(self = [super init])) return nil;
    __weak JTGrepperForm* weakSelf = self;
    _grepper = new JsonGrepper(
        JsonParserFromSettings(), true, CHECKPOINT_COUNT,
        [weakSelf](int64_t totalNumber, int64_t totalLengthOnHardDrive) {
            JTGrepperForm* self2 = weakSelf;
            if (self2)
                dispatch_async(dispatch_get_main_queue(), ^{
                  [self2 progressSetup:totalNumber lengthOnDisk:totalLengthOnHardDrive];
                });
        },
        [weakSelf](int64_t soFar, int64_t total) {
            JTGrepperForm* self2 = weakSelf;
            if (self2)
                dispatch_async(dispatch_get_main_queue(), ^{
                  [self2 progressReport:soFar total:total];
                });
        },
        [weakSelf] {
            JTGrepperForm* self2 = weakSelf;
            if (self2)
                dispatch_async(dispatch_get_main_queue(), ^{
                  [self2 progressTeardown];
                });
        });
    _filesFoundList = [NSMutableArray array];
    std::string configSubdir = ConfigSubDirectory();
    _directoriesVisitedFile = configSubdir + "/JsonTools directories visited.txt";
    _urlsQueriedFile = configSubdir + "/JsonTools urls queried.txt";
    [self buildWindow];
    [self loadHistory];
    return self;
}

- (void)dealloc {
    delete _grepper;
}

- (void)buildWindow {
    NSWindow* window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 560, 560)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    window.title = @"JSON from files and APIs";
    window.delegate = self;
    NSView* c = window.contentView;
    auto label = [&](NSString* text, CGFloat x, CGFloat y, CGFloat w) {
        NSTextField* l = [NSTextField labelWithString:text];
        l.frame = NSMakeRect(x, y, w, 18);
        l.font = [NSFont systemFontOfSize:11];
        l.autoresizingMask = NSViewMinYMargin;
        [c addSubview:l];
    };
    auto multiline = [&](CGFloat x, CGFloat y, CGFloat w, CGFloat h) {
        NSScrollView* sv = [[NSScrollView alloc] initWithFrame:NSMakeRect(x, y, w, h)];
        sv.hasVerticalScroller = YES;
        sv.borderType = NSBezelBorder;
        sv.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
        NSTextView* tvw = [[NSTextView alloc] init];
        tvw.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
        tvw.richText = NO;
        tvw.automaticQuoteSubstitutionEnabled = NO;
        tvw.minSize = NSMakeSize(0, 0);
        tvw.maxSize = NSMakeSize(FLT_MAX, FLT_MAX);
        tvw.verticallyResizable = YES;
        tvw.autoresizingMask = NSViewWidthSizable;
        sv.documentView = tvw;
        [c addSubview:sv];
        return tvw;
    };
    label(@"Enter URLs of APIs you want to request JSON from (one per line or a JSON array)", 12,
          532, 540);
    _urlsBox = multiline(12, 462, 420, 66);
    NSButton* sendBtn = [NSButton buttonWithTitle:@"Send API requests"
                                           target:self
                                           action:@selector(sendRequestsClicked:)];
    sendBtn.frame = NSMakeRect(438, 480, 116, 48);
    sendBtn.autoresizingMask = NSViewMinXMargin | NSViewMinYMargin;
    [c addSubview:sendBtn];

    label(@"Directory to search", 12, 436, 200);
    NSComboBox* dirBox = [[NSComboBox alloc] initWithFrame:NSMakeRect(12, 408, 420, 24)];
    dirBox.usesDataSource = NO;
    dirBox.completes = YES;
    dirBox.font = [NSFont systemFontOfSize:11];
    dirBox.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
    _directoriesBox = dirBox;
    [c addSubview:dirBox];
    NSButton* chooseBtn = [NSButton buttonWithTitle:@"Choose..."
                                             target:self
                                             action:@selector(chooseDirectoryClicked:)];
    chooseBtn.frame = NSMakeRect(438, 406, 116, 28);
    chooseBtn.autoresizingMask = NSViewMinXMargin | NSViewMinYMargin;
    [c addSubview:chooseBtn];

    label(@"Search patterns (e.g. *.json; one per line)", 12, 382, 300);
    _searchPatternsBox = multiline(12, 322, 300, 58);
    _searchPatternsBox.string = @"*.json\n*.jsonl";
    _recursiveCheck = [NSButton checkboxWithTitle:@"Search in subdirectories?"
                                           target:nil
                                           action:nil];
    _recursiveCheck.frame = NSMakeRect(320, 352, 200, 20);
    _recursiveCheck.autoresizingMask = NSViewMinXMargin | NSViewMinYMargin;
    [c addSubview:_recursiveCheck];
    NSButton* searchBtn = [NSButton buttonWithTitle:@"Search directories"
                                             target:self
                                             action:@selector(searchDirectoriesClicked:)];
    searchBtn.frame = NSMakeRect(320, 320, 160, 30);
    searchBtn.autoresizingMask = NSViewMinXMargin | NSViewMinYMargin;
    [c addSubview:searchBtn];

    label(@"Files and URLs with JSON", 12, 296, 300);
    NSScrollView* sv = [[NSScrollView alloc] initWithFrame:NSMakeRect(12, 56, 542, 238)];
    sv.hasVerticalScroller = YES;
    sv.borderType = NSBezelBorder;
    sv.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    NSTableView* table = [[NSTableView alloc] init];
    NSTableColumn* col = [[NSTableColumn alloc] initWithIdentifier:@"fname"];
    col.width = 520;
    [table addTableColumn:col];
    table.headerView = nil;
    table.allowsMultipleSelection = YES;
    table.dataSource = (id<NSTableViewDataSource>)self;
    table.delegate = (id<NSTableViewDelegate>)self;
    sv.documentView = table;
    _filesFoundTable = table;
    [c addSubview:sv];

    NSButton* viewResultsBtn = [NSButton buttonWithTitle:@"View results in buffer"
                                                  target:self
                                                  action:@selector(viewResultsClicked:)];
    viewResultsBtn.frame = NSMakeRect(12, 14, 170, 32);
    [c addSubview:viewResultsBtn];
    NSButton* removeBtn = [NSButton buttonWithTitle:@"Remove selected files"
                                             target:self
                                             action:@selector(removeSelectedClicked:)];
    removeBtn.frame = NSMakeRect(190, 14, 170, 32);
    [c addSubview:removeBtn];
    NSButton* errorsBtn = [NSButton buttonWithTitle:@"View errors"
                                             target:self
                                             action:@selector(viewErrorsClicked:)];
    errorsBtn.frame = NSMakeRect(368, 14, 110, 32);
    [c addSubview:errorsBtn];
    NSButton* docsBtn = [NSButton buttonWithTitle:@"Docs" target:self
                                           action:@selector(docsClicked:)];
    docsBtn.frame = NSMakeRect(484, 14, 70, 32);
    [c addSubview:docsBtn];
    [window center];
    _window = window;
}

// ── history persistence ──────────────────────────────────────────────────────
- (void)loadHistory {
    std::ifstream dirs(_directoriesVisitedFile);
    if (dirs) {
        std::string line;
        while (std::getline(dirs, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
            if (line.empty()) continue;
            BOOL isDir = NO;
            if ([[NSFileManager defaultManager] fileExistsAtPath:U8g(line) isDirectory:&isDir] &&
                isDir)
                [_directoriesBox addItemWithObjectValue:U8g(line)];
        }
        if (_directoriesBox.numberOfItems > 0)
            _directoriesBox.stringValue =
                [_directoriesBox itemObjectValueAtIndex:_directoriesBox.numberOfItems - 1];
    }
    std::ifstream urls(_urlsQueriedFile);
    if (urls) {
        std::ostringstream ss;
        ss << urls.rdbuf();
        std::string text = ss.str();
        _urlsBox.string = U8g(text);
        std::istringstream lines(text);
        std::string line;
        while (std::getline(lines, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
            if (!line.empty()) _urlsQueried.push_back(line);
        }
    }
}

- (void)saveHistory {
    if (_directoriesBox.numberOfItems > 0) {
        std::ofstream fp(_directoriesVisitedFile, std::ios::trunc);
        if (fp) {
            for (NSInteger i = 0; i < _directoriesBox.numberOfItems; i++) {
                NSString* d = [_directoriesBox itemObjectValueAtIndex:i];
                fp << (d.UTF8String ?: "") << "\n";
            }
        }
    }
    if (!_urlsQueried.empty()) {
        std::ofstream fp(_urlsQueriedFile, std::ios::trunc);
        if (fp)
            for (const std::string& u : _urlsQueried) fp << u << "\r\n";
    }
}

- (void)addDirectoryToVisited:(NSString*)rootDir {
    BOOL isDir = NO;
    if (![[NSFileManager defaultManager] fileExistsAtPath:rootDir isDirectory:&isDir] || !isDir)
        return;
    if ([_directoriesBox indexOfItemWithObjectValue:rootDir] != NSNotFound) return;
    [_directoriesBox addItemWithObjectValue:rootDir];
    if (_directoriesBox.numberOfItems > 10) [_directoriesBox removeItemAtIndex:0];
}

// ── table datasource ─────────────────────────────────────────────────────────
- (NSInteger)numberOfRowsInTableView:(NSTableView*)tv {
    return (NSInteger)_filesFoundList.count;
}

- (NSView*)tableView:(NSTableView*)tv viewForTableColumn:(NSTableColumn*)col row:(NSInteger)row {
    NSTextField* tf = [tv makeViewWithIdentifier:@"grepcell" owner:self];
    if (!tf) {
        tf = [NSTextField labelWithString:@""];
        tf.identifier = @"grepcell";
        tf.font = [NSFont systemFontOfSize:[NSFont smallSystemFontSize]];
        tf.lineBreakMode = NSLineBreakByTruncatingMiddle;
    }
    tf.stringValue = _filesFoundList[(NSUInteger)row];
    return tf;
}

// ── progress sheet ───────────────────────────────────────────────────────────
- (void)progressSetup:(int64_t)totalNumber lengthOnDisk:(int64_t)totalLengthOnHardDrive {
    bool isParsing = totalLengthOnHardDrive == -1;
    if (!_progressPanel) {
        NSPanel* panel = [[NSPanel alloc]
            initWithContentRect:NSMakeRect(0, 0, 420, 130)
                      styleMask:NSWindowStyleMaskTitled
                        backing:NSBackingStoreBuffered
                          defer:NO];
        NSView* c = panel.contentView;
        NSTextField* lbl = [NSTextField labelWithString:@""];
        lbl.frame = NSMakeRect(16, 86, 388, 20);
        [c addSubview:lbl];
        _progressLabel = lbl;
        NSProgressIndicator* bar = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(16, 56, 388, 20)];
        bar.indeterminate = NO;
        [c addSubview:bar];
        _progressBar = bar;
        NSButton* cancel = [NSButton buttonWithTitle:@"Cancel"
                                              target:self
                                              action:@selector(cancelGrepClicked:)];
        cancel.frame = NSMakeRect(160, 12, 100, 30);
        [c addSubview:cancel];
        _progressPanel = panel;
    }
    _progressPanel.title = isParsing ? @"JSON parsing in progress" : @"File reading in progress";
    _progressBar.minValue = 0;
    _progressBar.maxValue = (double)totalNumber;
    _progressBar.doubleValue = 0;
    _progressLabel.stringValue = isParsing
        ? [NSString stringWithFormat:@"Parsing documents with combined length of about %.3f MB",
                                     totalNumber / 1e6]
        : [NSString stringWithFormat:@"Reading %lld files (about %.3f MB)",
                                     (long long)totalNumber, totalLengthOnHardDrive / 1e6];
    [_window beginSheet:_progressPanel completionHandler:nil];
}

- (void)progressReport:(int64_t)soFar total:(int64_t)total {
    if (_progressBar) _progressBar.doubleValue = (double)soFar;
}

- (void)progressTeardown {
    if (_progressPanel) {
        [_window endSheet:_progressPanel];
        [_progressPanel orderOut:nil];
    }
}

- (void)cancelGrepClicked:(id)sender {
    _grepper->Cancel();
}

// ── actions ──────────────────────────────────────────────────────────────────
- (void)runGrepJob:(void (^)(void))job {
    if (_busy) return;
    _busy = true;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      job();
      dispatch_async(dispatch_get_main_queue(), ^{
        self.busy = false;
        [self progressTeardown]; // in case no teardown callback fired
        if (self->_grepper->tooMuchText)
            MsgBoxError("Too much text to parse",
                        "The total length of text to be parsed exceeded the maximum length (" +
                            std::to_string(JsonGrepper::MAX_COMBINED_LENGTH_TEXT_TO_PARSE) + ")");
        [self addFilesToFilesFound];
      });
    });
}

- (void)sendRequestsClicked:(id)sender {
    std::vector<std::string> urls;
    std::string urlsText = _urlsBox.string.UTF8String ?: "";
    bool parsedAsJson = false;
    try {
        JsonParser parser(LoggerLevel::JSON5);
        JNodePtr parsed = parser.Parse(urlsText);
        if (JArray* arr = AsArr(parsed)) {
            for (JNodePtr& node : arr->children)
                if (node->IsStrValue()) urls.push_back(node->GetStr());
            parsedAsJson = true;
        }
    } catch (...) {
    }
    if (!parsedAsJson) {
        std::istringstream lines(urlsText);
        std::string line;
        while (std::getline(lines, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
            if (!line.empty()) urls.push_back(line);
        }
    }
    if (urls.empty()) return;
    JsonGrepper* grepper = _grepper;
    [self runGrepJob:^{
      try {
          grepper->GetJsonFromApis(urls);
      } catch (...) {
      }
    }];
}

- (void)chooseDirectoryClicked:(id)sender {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.canChooseFiles = NO;
    panel.canChooseDirectories = YES;
    panel.allowsMultipleSelection = NO;
    if ([panel runModal] == NSModalResponseOK && panel.URL) {
        _directoriesBox.stringValue = panel.URL.path;
        [self addDirectoryToVisited:panel.URL.path];
    }
}

- (void)searchDirectoriesClicked:(id)sender {
    std::vector<std::string> searchPatterns;
    std::istringstream lines(_searchPatternsBox.string.UTF8String ?: "");
    std::string line;
    while (std::getline(lines, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (!line.empty()) searchPatterns.push_back(line);
    }
    if (searchPatterns.empty()) searchPatterns.push_back("*.json");
    bool recursive = _recursiveCheck.state == NSControlStateValueOn;
    std::string rootDir = _directoriesBox.stringValue.UTF8String ?: "";
    [self addDirectoryToVisited:_directoriesBox.stringValue];
    BOOL isDir = NO;
    if (![[NSFileManager defaultManager] fileExistsAtPath:U8g(rootDir) isDirectory:&isDir] || !isDir)
        return;
    JsonGrepper* grepper = _grepper;
    [self runGrepJob:^{
      try {
          grepper->Grep(rootDir, recursive, searchPatterns);
      } catch (...) {
      }
    }];
}

- (void)addFilesToFilesFound {
    for (auto& kv : _grepper->fnameJsons->children) {
        const std::string& fname = kv.first;
        if (!_filesFound.count(fname)) {
            [_filesFoundList addObject:U8g(fname)];
            _filesFound.insert(fname);
        }
        if ((fname.rfind("http://", 0) == 0 || fname.rfind("https://", 0) == 0) &&
            std::find(_urlsQueried.begin(), _urlsQueried.end(), fname) == _urlsQueried.end()) {
            _urlsQueried.push_back(fname);
            if (_urlsQueried.size() > 10) _urlsQueried.erase(_urlsQueried.begin());
        }
    }
    [_filesFoundTable reloadData];
}

- (void)viewErrorsClicked:(id)sender {
    if (_grepper->exceptions->Length() == 0) {
        MsgBoxInfo("No errors while searching documents", "No exceptions! \U0001F600");
        return;
    }
    PrettyPrintJsonInNewFile(_grepper->exceptions);
}

- (void)removeSelectedClicked:(id)sender {
    grepperTreeViewJustOpened = true;
    NSIndexSet* selected = _filesFoundTable.selectedRowIndexes;
    NSMutableArray<NSString*>* toRemove = [NSMutableArray array];
    [selected enumerateIndexesUsingBlock:^(NSUInteger idx, BOOL* stop) {
      [toRemove addObject:self->_filesFoundList[idx]];
    }];
    for (NSString* file in toRemove) {
        std::string fname = file.UTF8String ?: "";
        [_filesFoundList removeObject:file];
        _filesFound.erase(fname);
        _grepper->fnameJsons->children.erase(fname);
    }
    [_filesFoundTable reloadData];
    if (_tv) {
        [_tv setJson:_grepper->fnameJsons];
        std::string fileOpen = GetCurrentFilePath();
        OpenFile([_tv fname]);
        SetText(PrettyPrintFromSettings(_grepper->fnameJsons));
        ScrollCaret();
        [_tv submitQuery];
        OpenFile(fileOpen);
        if (openTreeViewer) [openTreeViewer hidePanel];
    }
}

- (void)viewResultsClicked:(id)sender {
    grepperTreeViewJustOpened = true;
    PrettyPrintJsonInNewFile(_grepper->fnameJsons);
    if (_tv) {
        [_tv closePanel];
        _tv = nil;
    }
    _tv = [[JTTreeViewer alloc] initWithJson:_grepper->fnameJsons];
    _tv.isGrepperTree = true;
    DisplayJsonTree(_tv, _grepper->fnameJsons, "JSON from files and APIs tree", false,
                    DocumentType::JSON, false);
    if (openTreeViewer) [openTreeViewer hidePanel];
}

- (void)docsClicked:(id)sender {
    OpenUrlInWebBrowser(
        "https://github.com/molsonkiko/JsonToolsNppPlugin/blob/main/docs/README.md#get-json-from-files-and-apis");
}

// ── closing ──────────────────────────────────────────────────────────────────
- (void)windowWillClose:(NSNotification*)notification {
    [self closeFormInternal];
}

- (void)closeFormInternal {
    if (_grepper) {
        _grepper->Cancel();
        _grepper->Reset();
    }
    if (_progressPanel) [self progressTeardown];
    if (_tv) {
        [_tv closePanel];
        _tv = nil;
    }
    if (openTreeViewer) [openTreeViewer showPanel];
    [self saveHistory];
    if (sGrepperForm == self) sGrepperForm = nil;
}

- (void)closeForm {
    [_window close]; // triggers windowWillClose
}

- (void)applyStyling {
    [_filesFoundTable reloadData];
    if (_tv) [_tv applyStyling];
}

@end
