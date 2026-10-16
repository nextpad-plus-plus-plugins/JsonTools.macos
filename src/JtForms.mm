// JsonTools.macos — JtForms.mm: see JtForms.h.

#import "JtForms.h"

#include "JsonSchemaMaker.h"
#include "JsonTabularize.h"
#include "MiscModules.h"
#include "RemesPath.h"

#include "JtCore.h"
#include "JtNpp.h"
#include "JtSettings.h"
#import "TreeViewer.h"

#include <random>
#include <sstream>

using namespace jsontools;
using namespace jtnpp;
using namespace jtcore;

extern int jtFuncItemCmdId(int funcItemIndex);
extern const int JT_ERROR_FORM_FUNC_INDEX;

static NSString* U8(const std::string& s) {
    return [NSString stringWithUTF8String:s.c_str()] ?: @"";
}

// ── About dialog ─────────────────────────────────────────────────────────────
void jtShowAboutDialog(void) {
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = @"JsonTools";
    alert.informativeText =
        @"Query/editing tool for JSON, including linting, reformatting, a tree viewer with "
        @"path-to-node, the RemesPath query language, JSON-schema validation and generation, "
        @"random-JSON generation, and more.\n\n"
        @"macOS port (v" JSONTOOLS_VERSION ") of molsonkiko's JsonTools Notepad++ plugin.\n"
        @"Original plugin: https://github.com/molsonkiko/JsonToolsNppPlugin";
    [alert addButtonWithTitle:@"OK"];
    [alert addButtonWithTitle:@"Open documentation"];
    if ([alert runModal] == NSAlertSecondButtonReturn)
        OpenUrlInWebBrowser("https://github.com/molsonkiko/JsonToolsNppPlugin");
}

// ── Settings dialog ──────────────────────────────────────────────────────────
// a property-grid-style modal dialog: rows grouped by category, with a
// description pane at the bottom (like the WinForms PropertyGrid)

namespace {

enum class SettingKind { Bool, Int, Float, Double, String, Enum };

struct SettingRow {
    const char* name;
    const char* category;
    SettingKind kind;
    const char* description;
    std::vector<const char*> enumNames; // for Enum
};

} // namespace

@interface JTSettingsDialog : NSObject
@end

@implementation JTSettingsDialog
@end

static const std::vector<SettingRow>& settingRows() {
    static std::vector<SettingRow> rows = {
        {"ask_before_pretty_printing_json_lines", "JSON formatting", SettingKind::Enum,
         "Ask before pretty-printing JSON Lines documents, ignore requests to pretty-print, or "
         "pretty-print without asking?",
         {"DONT_DO_DONT_ASK", "ASK_BEFORE_DOING", "DO_WITHOUT_ASKING"}},
        {"indent_pretty_print", "JSON formatting", SettingKind::Int,
         "The number of spaces between levels of JSON when pretty-printing", {}},
        {"minimal_whitespace_compression", "JSON formatting", SettingKind::Bool,
         "If true, the 'Compress JSON' command removes ALL unnecessary whitespace; otherwise it "
         "leaves space after ':' in objects and after ',' in both objects and arrays", {}},
        {"pretty_print_style", "JSON formatting", SettingKind::Enum,
         "How JSON is pretty printed (Google, Whitesmith, or PPrint style)",
         {"Google", "Whitesmith", "PPrint"}},
        {"remember_comments", "JSON formatting", SettingKind::Bool,
         "When JSON is pretty-printed or compressed, comments found when it was last parsed are "
         "included", {}},
        {"sort_keys", "JSON formatting", SettingKind::Bool,
         "Sort the keys of objects alphabetically when pretty-printing or compressing", {}},
        {"tab_indent_pretty_print", "JSON formatting", SettingKind::Bool,
         "Use one horizontal tab instead of spaces between levels of JSON when pretty-printing",
         {}},
        {"logger_level", "JSON Parser", SettingKind::Enum,
         "Suppress logging of errors at or below this level (STRICT < OK < NAN_INF < JSONC < "
         "JSON5)",
         {"STRICT", "OK", "NAN_INF", "JSONC", "JSON5"}},
        {"offer_to_show_lint", "JSON Parser", SettingKind::Bool,
         "When the document is parsed, show a prompt to see syntax errors in the document", {}},
        {"max_schema_validation_problems", "JSON Schema", SettingKind::Int,
         "Maximum number of JSON Schema validation problems to log before the validator stops",
         {}},
        {"auto_try_guess_csv_delim_newline", "Miscellaneous", SettingKind::Bool,
         "When the regex search form is opened, try to guess whether the current document is a "
         "CSV or TSV file, and how many columns and what newline it has", {}},
        {"csv_newline", "Miscellaneous", SettingKind::Enum,
         "Which type of newline to use for generated CSV files", {"CRLF", "CR", "LF"}},
        {"key_style", "Miscellaneous", SettingKind::Enum,
         "The style of key to use when getting the path or key/index of a node or line",
         {"JavaScript", "Python", "RemesPath"}},
        {"keyboard_shortcuts", "Miscellaneous", SettingKind::Bool,
         "Whether JsonTools automatically assigns default keyboard shortcuts to some plugin "
         "commands at startup (takes effect at the next startup)", {}},
        {"path_separator", "Miscellaneous", SettingKind::String,
         "The separator to use when formatting a path, as a JSON string. Ignored when set to "
         "\"\\u0001\" (the default). Must be exactly one character, and cannot be any of "
         "\"0123456789 or a double quote", {}},
        {"skip_api_request_and_fuzz_tests", "Miscellaneous", SettingKind::Bool,
         "When running tests, skip the tests that send requests to APIs and the fuzz tests", {}},
        {"toolbar_icons", "Miscellaneous", SettingKind::String,
         "Specify one of these chars for each toolbar icon you want to show, in the order you "
         "want: 't' = tree view, 'c' = compress, 'p' = pretty-print, 'o' = path to current "
         "position. Takes effect at the next startup.", {}},
        {"try_parse_start_chars", "Miscellaneous", SettingKind::String,
         "When selecting every JSON in the file, start trying to parse only at these characters",
         {}},
        {"auto_validate", "Performance", SettingKind::Bool,
         "Automatically validate .json, .jsonc, and .jsonl files every couple of seconds, except "
         "very large files", {}},
        {"inactivity_seconds_before_parse", "Performance", SettingKind::Int,
         "How many seconds of user inactivity before the plugin re-parses the document. Minimum "
         "1.", {}},
        {"max_file_size_MB_slow_actions", "Performance", SettingKind::Double,
         "Files larger than this number of megabytes have slow actions (auto-parse etc.) "
         "disabled", {}},
        {"extended_ascii_strings", "Random JSON", SettingKind::Bool,
         "Use extended ASCII characters in random strings?", {}},
        {"generate_random_patterns", "Random JSON", SettingKind::Bool,
         "Generate random strings matching the \"pattern\" keyword (NOT SUPPORTED in the macOS "
         "port; patterns are ignored)", {}},
        {"maxArrayLength", "Random JSON", SettingKind::Int,
         "Maximum length of random arrays, unless specified by the \"maxItems\" keyword", {}},
        {"minArrayLength", "Random JSON", SettingKind::Int,
         "Minimum length of random arrays, unless specified by the \"minItems\" keyword", {}},
        {"tree_view_font_size", "Styling", SettingKind::Float,
         "The font size (in points) for the tree viewer", {}},
        {"use_npp_styling", "Styling", SettingKind::Bool,
         "Use the editor theme's colors for the tree viewer and other JsonTools forms?", {}},
        {"max_json_length_full_tree", "Tree View", SettingKind::Int,
         "The longest length of a JSON array or object that gets all its children added to the "
         "tree view; longer iterables get only some children added", {}},
        {"tree_node_images", "Tree View", SettingKind::Bool,
         "Should each node in the tree have an image associated with its type?", {}},
    };
    return rows;
}

// get/set a setting by name as a display string
static std::string settingGet(const std::string& name) {
    JtSettings& s = jtSettings();
    auto b = [](bool v) { return std::string(v ? "True" : "False"); };
    if (name == "logger_level") {
        static const char* n[] = {"STRICT", "OK", "NAN_INF", "JSONC", "JSON5"};
        return n[(int)s.logger_level];
    }
    if (name == "offer_to_show_lint") return b(s.offer_to_show_lint);
    if (name == "max_file_size_MB_slow_actions") {
        std::ostringstream os; os << s.max_file_size_MB_slow_actions; return os.str();
    }
    if (name == "auto_validate") return b(s.auto_validate);
    if (name == "inactivity_seconds_before_parse") return std::to_string(s.inactivity_seconds_before_parse);
    if (name == "max_json_length_full_tree") return std::to_string(s.max_json_length_full_tree);
    if (name == "tree_node_images") return b(s.tree_node_images);
    if (name == "indent_pretty_print") return std::to_string(s.indent_pretty_print);
    if (name == "tab_indent_pretty_print") return b(s.tab_indent_pretty_print);
    if (name == "minimal_whitespace_compression") return b(s.minimal_whitespace_compression);
    if (name == "sort_keys") return b(s.sort_keys);
    if (name == "pretty_print_style") {
        static const char* n[] = {"Google", "Whitesmith", "PPrint"};
        return n[(int)s.pretty_print_style];
    }
    if (name == "remember_comments") return b(s.remember_comments);
    if (name == "ask_before_pretty_printing_json_lines") {
        static const char* n[] = {"DONT_DO_DONT_ASK", "ASK_BEFORE_DOING", "DO_WITHOUT_ASKING"};
        return n[(int)s.ask_before_pretty_printing_json_lines];
    }
    if (name == "key_style") {
        static const char* n[] = {"JavaScript", "Python", "RemesPath"};
        return n[(int)s.key_style];
    }
    if (name == "keyboard_shortcuts") return b(s.keyboard_shortcuts);
    if (name == "path_separator") return s.path_separator;
    if (name == "try_parse_start_chars") return s.try_parse_start_chars;
    if (name == "skip_api_request_and_fuzz_tests") return b(s.skip_api_request_and_fuzz_tests);
    if (name == "csv_newline") {
        static const char* n[] = {"CRLF", "CR", "LF"};
        return n[(int)s.csv_newline];
    }
    if (name == "toolbar_icons") return s.toolbar_icons;
    if (name == "auto_try_guess_csv_delim_newline") return b(s.auto_try_guess_csv_delim_newline);
    if (name == "use_npp_styling") return b(s.use_npp_styling);
    if (name == "tree_view_font_size") {
        std::ostringstream os; os << s.tree_view_font_size; return os.str();
    }
    if (name == "minArrayLength") return std::to_string(s.minArrayLength);
    if (name == "maxArrayLength") return std::to_string(s.maxArrayLength);
    if (name == "extended_ascii_strings") return b(s.extended_ascii_strings);
    if (name == "generate_random_patterns") return b(s.generate_random_patterns);
    if (name == "max_schema_validation_problems") return std::to_string(s.max_schema_validation_problems);
    return "";
}

static void settingSet(const std::string& name, const std::string& value) {
    JtSettings& s = jtSettings();
    auto toBool = [&] { return value == "True" || value == "true" || value == "1"; };
    auto toInt = [&](int fb) { try { return std::stoi(value); } catch (...) { return fb; } };
    auto toDouble = [&](double fb) { try { return std::stod(value); } catch (...) { return fb; } };
    auto enumIdx = [&](std::initializer_list<const char*> names, int fb) {
        int i = 0;
        for (const char* n : names) {
            if (value == n) return i;
            i++;
        }
        return fb;
    };
    if (name == "logger_level")
        s.logger_level = (LoggerLevel)enumIdx({"STRICT", "OK", "NAN_INF", "JSONC", "JSON5"},
                                              (int)s.logger_level);
    else if (name == "offer_to_show_lint") s.offer_to_show_lint = toBool();
    else if (name == "max_file_size_MB_slow_actions")
        s.max_file_size_MB_slow_actions = toDouble(s.max_file_size_MB_slow_actions);
    else if (name == "auto_validate") s.auto_validate = toBool();
    else if (name == "inactivity_seconds_before_parse")
        s.inactivity_seconds_before_parse = toInt(s.inactivity_seconds_before_parse);
    else if (name == "max_json_length_full_tree")
        s.max_json_length_full_tree = toInt(s.max_json_length_full_tree);
    else if (name == "tree_node_images") s.tree_node_images = toBool();
    else if (name == "indent_pretty_print") s.indent_pretty_print = toInt(s.indent_pretty_print);
    else if (name == "tab_indent_pretty_print") s.tab_indent_pretty_print = toBool();
    else if (name == "minimal_whitespace_compression") s.minimal_whitespace_compression = toBool();
    else if (name == "sort_keys") s.sort_keys = toBool();
    else if (name == "pretty_print_style")
        s.pretty_print_style =
            (PrettyPrintStyle)enumIdx({"Google", "Whitesmith", "PPrint"}, (int)s.pretty_print_style);
    else if (name == "remember_comments") s.remember_comments = toBool();
    else if (name == "ask_before_pretty_printing_json_lines")
        s.ask_before_pretty_printing_json_lines = (AskUserWhetherToDoThing)enumIdx(
            {"DONT_DO_DONT_ASK", "ASK_BEFORE_DOING", "DO_WITHOUT_ASKING"},
            (int)s.ask_before_pretty_printing_json_lines);
    else if (name == "key_style")
        s.key_style = (KeyStyle)enumIdx({"JavaScript", "Python", "RemesPath"}, (int)s.key_style);
    else if (name == "keyboard_shortcuts") s.keyboard_shortcuts = toBool();
    else if (name == "path_separator") s.path_separator = value;
    else if (name == "try_parse_start_chars") s.try_parse_start_chars = value;
    else if (name == "skip_api_request_and_fuzz_tests") s.skip_api_request_and_fuzz_tests = toBool();
    else if (name == "csv_newline")
        s.csv_newline = (EndOfLine)enumIdx({"CRLF", "CR", "LF"}, (int)s.csv_newline);
    else if (name == "toolbar_icons") s.toolbar_icons = value;
    else if (name == "auto_try_guess_csv_delim_newline") s.auto_try_guess_csv_delim_newline = toBool();
    else if (name == "use_npp_styling") s.use_npp_styling = toBool();
    else if (name == "tree_view_font_size")
        s.tree_view_font_size = (float)toDouble(s.tree_view_font_size);
    else if (name == "minArrayLength") s.minArrayLength = toInt(s.minArrayLength);
    else if (name == "maxArrayLength") s.maxArrayLength = toInt(s.maxArrayLength);
    else if (name == "extended_ascii_strings") s.extended_ascii_strings = toBool();
    else if (name == "generate_random_patterns") s.generate_random_patterns = toBool();
    else if (name == "max_schema_validation_problems")
        s.max_schema_validation_problems = toInt(s.max_schema_validation_problems);
}

bool jtRunSettingsDialog(void) {
    const auto& rows = settingRows();
    NSWindow* window =
        [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 620, 640)
                                    styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                              NSWindowStyleMaskResizable
                                      backing:NSBackingStoreBuffered
                                        defer:NO];
    window.title = @"Settings - JsonTools plug-in";
    NSView* content = window.contentView;

    NSTextField* descLabel = [NSTextField wrappingLabelWithString:@""];
    descLabel.translatesAutoresizingMaskIntoConstraints = NO;
    descLabel.font = [NSFont systemFontOfSize:[NSFont smallSystemFontSize]];
    descLabel.selectable = NO;

    // grid of rows grouped by category
    NSView* grid = [[NSView alloc] init];
    grid.translatesAutoresizingMaskIntoConstraints = NO;
    NSMutableDictionary<NSString*, NSControl*>* controls = [NSMutableDictionary dictionary];
    CGFloat y = 8;
    std::string lastCategory;
    NSMutableArray<NSView*>* gridSubviews = [NSMutableArray array];
    // build top-down; collect heights first
    for (const SettingRow& row : rows) {
        if (lastCategory != row.category) {
            lastCategory = row.category;
            NSTextField* cat = [NSTextField labelWithString:U8(row.category)];
            cat.font = [NSFont boldSystemFontOfSize:12];
            cat.frame = NSMakeRect(8, 0, 300, 20);
            [gridSubviews addObject:cat];
        }
        NSTextField* nameLabel = [NSTextField labelWithString:U8(row.name)];
        nameLabel.font = [NSFont systemFontOfSize:11];
        nameLabel.frame = NSMakeRect(24, 0, 276, 18);
        nameLabel.toolTip = U8(row.description);
        [gridSubviews addObject:nameLabel];
        NSControl* control;
        std::string current = settingGet(row.name);
        if (row.kind == SettingKind::Bool) {
            NSPopUpButton* popup = [[NSPopUpButton alloc] init];
            [popup addItemsWithTitles:@[ @"True", @"False" ]];
            [popup selectItemWithTitle:U8(current)];
            control = popup;
        } else if (row.kind == SettingKind::Enum) {
            NSPopUpButton* popup = [[NSPopUpButton alloc] init];
            for (const char* n : row.enumNames) [popup addItemWithTitle:U8(n)];
            [popup selectItemWithTitle:U8(current)];
            control = popup;
        } else {
            NSTextField* tf = [[NSTextField alloc] init];
            tf.stringValue = U8(current);
            tf.font = [NSFont systemFontOfSize:11];
            control = tf;
        }
        control.frame = NSMakeRect(306, 0, 280, 20);
        ((NSControl*)control).controlSize = NSControlSizeSmall;
        control.toolTip = U8(row.description);
        controls[U8(row.name)] = control;
        [gridSubviews addObject:control];
    }
    // lay out bottom-up (AppKit coordinates): walk the collected views in
    // reverse, stacking labels/controls on shared rows
    for (NSInteger i = (NSInteger)gridSubviews.count - 1; i >= 0; i--) {
        NSView* v = gridSubviews[(NSUInteger)i];
        NSRect f = v.frame;
        if ([v isKindOfClass:[NSControl class]] && f.origin.x > 300) {
            // value control: same row as the preceding name label
            f.origin.y = y;
            v.frame = f;
            NSView* label = gridSubviews[(NSUInteger)i - 1];
            NSRect lf = label.frame;
            lf.origin.y = y + 2;
            label.frame = lf;
            [grid addSubview:label];
            [grid addSubview:v];
            y += 26;
            i--; // skip the label we just placed
        } else {
            // category header
            f.origin.y = y + 4;
            v.frame = f;
            [grid addSubview:v];
            y += 30;
        }
    }
    grid.frame = NSMakeRect(0, 0, 600, y + 8);

    NSScrollView* sv = [[NSScrollView alloc] init];
    sv.translatesAutoresizingMaskIntoConstraints = NO;
    sv.hasVerticalScroller = YES;
    sv.documentView = grid;
    [content addSubview:sv];
    [content addSubview:descLabel];

    NSButton* okBtn = [NSButton buttonWithTitle:@"OK" target:nil action:@selector(stopModal)];
    okBtn.target = NSApp;
    okBtn.keyEquivalent = @"\r";
    okBtn.translatesAutoresizingMaskIntoConstraints = NO;
    NSButton* cancelBtn = [NSButton buttonWithTitle:@"Cancel" target:nil
                                             action:@selector(abortModal)];
    cancelBtn.target = NSApp;
    cancelBtn.keyEquivalent = @"\033";
    cancelBtn.translatesAutoresizingMaskIntoConstraints = NO;
    [content addSubview:okBtn];
    [content addSubview:cancelBtn];
    [NSLayoutConstraint activateConstraints:@[
        [sv.topAnchor constraintEqualToAnchor:content.topAnchor constant:8],
        [sv.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:8],
        [sv.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-8],
        [sv.bottomAnchor constraintEqualToAnchor:descLabel.topAnchor constant:-8],
        [descLabel.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:12],
        [descLabel.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-12],
        [descLabel.bottomAnchor constraintEqualToAnchor:okBtn.topAnchor constant:-8],
        [descLabel.heightAnchor constraintGreaterThanOrEqualToConstant:34],
        [okBtn.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-12],
        [okBtn.bottomAnchor constraintEqualToAnchor:content.bottomAnchor constant:-10],
        [cancelBtn.trailingAnchor constraintEqualToAnchor:okBtn.leadingAnchor constant:-8],
        [cancelBtn.bottomAnchor constraintEqualToAnchor:okBtn.bottomAnchor],
    ]];
    descLabel.stringValue = @"Hover over a setting's name for its description.";
    [window center];
    NSModalResponse resp = [NSApp runModalForWindow:window];
    [window orderOut:nil];
    if (resp != NSModalResponseStop) return false; // Cancel (abortModal)
    bool changed = false;
    for (const SettingRow& row : rows) {
        NSControl* control = controls[U8(row.name)];
        std::string newValue;
        if ([control isKindOfClass:[NSPopUpButton class]])
            newValue = ((NSPopUpButton*)control).titleOfSelectedItem.UTF8String ?: "";
        else
            newValue = ((NSTextField*)control).stringValue.UTF8String ?: "";
        if (newValue != settingGet(row.name)) {
            settingSet(row.name, newValue);
            changed = true;
        }
    }
    if (changed) jtSettingsSave();
    return changed;
}

// ── error form (bottom panel) ────────────────────────────────────────────────
static const size_t LINT_MAX_ROW_COUNT = 5000;

@interface JTErrorForm : NSObject <NSTableViewDataSource, NSTableViewDelegate> {
  @public
    std::string fname;
    std::vector<JsonLint> lints;
    std::vector<size_t> shownRows; // indices into lints; SIZE_MAX = warning row
}
@property(nonatomic, strong) NSView* containerView;
@property(nonatomic, strong) NSTableView* table;
@property(nonatomic) uint64_t panelHandle;
@end

@implementation JTErrorForm

- (instancetype)init {
    if (!(self = [super init])) return nil;
    NSView* root = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 600, 150)];
    NSScrollView* sv = [[NSScrollView alloc] init];
    sv.translatesAutoresizingMaskIntoConstraints = NO;
    sv.hasVerticalScroller = YES;
    NSTableView* table = [[NSTableView alloc] init];
    NSTableColumn* sevCol = [[NSTableColumn alloc] initWithIdentifier:@"severity"];
    sevCol.title = @"Severity";
    sevCol.width = 75;
    NSTableColumn* descCol = [[NSTableColumn alloc] initWithIdentifier:@"description"];
    descCol.title = @"Description";
    descCol.width = 420;
    NSTableColumn* posCol = [[NSTableColumn alloc] initWithIdentifier:@"position"];
    posCol.title = @"Position";
    posCol.width = 75;
    [table addTableColumn:sevCol];
    [table addTableColumn:descCol];
    [table addTableColumn:posCol];
    table.dataSource = self;
    table.delegate = self;
    table.usesAlternatingRowBackgroundColors = YES;
    NSMenu* menu = [[NSMenu alloc] init];
    NSMenuItem* refresh = [menu addItemWithTitle:@"Refresh with current errors"
                                          action:@selector(refreshClicked:)
                                   keyEquivalent:@""];
    refresh.target = self;
    NSMenuItem* exportItem = [menu addItemWithTitle:@"Export to JSON"
                                             action:@selector(exportClicked:)
                                      keyEquivalent:@""];
    exportItem.target = self;
    table.menu = menu;
    sv.documentView = table;
    [root addSubview:sv];
    [NSLayoutConstraint activateConstraints:@[
        [sv.topAnchor constraintEqualToAnchor:root.topAnchor],
        [sv.bottomAnchor constraintEqualToAnchor:root.bottomAnchor],
        [sv.leadingAnchor constraintEqualToAnchor:root.leadingAnchor],
        [sv.trailingAnchor constraintEqualToAnchor:root.trailingAnchor],
    ]];
    _containerView = root;
    _table = table;
    _panelHandle = 0;
    return self;
}

- (void)reloadWithFname:(const std::string&)newFname lints:(const std::vector<JsonLint>&)newLints {
    fname = newFname;
    lints = newLints;
    shownRows.clear();
    size_t lintCount = lints.size();
    size_t interval = 1;
    if (LINT_MAX_ROW_COUNT < lintCount) {
        interval = lintCount / LINT_MAX_ROW_COUNT;
        shownRows.push_back(SIZE_MAX); // "not all rows shown" warning row
    }
    size_t ii = 0, cycler = 0;
    while (ii < lintCount) {
        shownRows.push_back(ii);
        if (interval == 1) {
            ii++;
        } else {
            ii += (cycler % (2 * interval)) + 1;
            cycler++;
        }
    }
    [_table reloadData];
}

- (NSInteger)numberOfRowsInTableView:(NSTableView*)tv {
    return (NSInteger)shownRows.size();
}

- (NSView*)tableView:(NSTableView*)tv viewForTableColumn:(NSTableColumn*)col row:(NSInteger)row {
    NSTextField* tf = [tv makeViewWithIdentifier:@"errcell" owner:self];
    if (!tf) {
        tf = [NSTextField labelWithString:@""];
        tf.identifier = @"errcell";
        tf.font = [NSFont systemFontOfSize:[NSFont smallSystemFontSize]];
        tf.lineBreakMode = NSLineBreakByTruncatingTail;
    }
    size_t idx = shownRows[(size_t)row];
    if (idx == SIZE_MAX) {
        if ([col.identifier isEqualToString:@"description"])
            tf.stringValue = [NSString
                stringWithFormat:@"Showing approximately %zu of %zu rows", LINT_MAX_ROW_COUNT,
                                 lints.size()];
        else
            tf.stringValue = @"";
        return tf;
    }
    const JsonLint& lint = lints[idx];
    if ([col.identifier isEqualToString:@"severity"])
        tf.stringValue = U8(JsonLint::ParserStateName(lint.severity()));
    else if ([col.identifier isEqualToString:@"description"])
        tf.stringValue = U8(lint.message());
    else
        tf.stringValue = [NSString stringWithFormat:@"%lld", (long long)lint.pos];
    return tf;
}

- (void)tableViewSelectionDidChange:(NSNotification*)note {
    NSInteger row = _table.selectedRow;
    if (row < 0 || (size_t)row >= shownRows.size()) return;
    size_t idx = shownRows[(size_t)row];
    if (idx == SIZE_MAX) return;
    GoToLegalPos(lints[idx].pos);
}

- (void)refreshClicked:(id)sender {
    // upstream ErrorForm.RefreshWithContentsOfCurrentFile
    errorFormTriggeredParse = true;
    bool previousOffer = jtSettings().offer_to_show_lint;
    jtSettings().offer_to_show_lint = false;
    TryParseJson(DocumentType::JSON, false, true);
    JsonFileInfo* info = nullptr;
    if (TryGetInfoForFile(activeFname, &info) && info->hasLints) {
        if (info->filenameOfMostRecentValidatingSchema.empty()) {
            [self reloadWithFname:activeFname lints:info->lints];
        } else {
            ValidateJson(info->filenameOfMostRecentValidatingSchema, false, false, false);
            if (TryGetInfoForFile(activeFname, &info) && info->hasLints)
                [self reloadWithFname:activeFname lints:info->lints];
        }
    }
    jtSettings().offer_to_show_lint = previousOffer;
    errorFormTriggeredParse = false;
}

- (void)exportClicked:(id)sender {
    if (lints.empty()) {
        MsgBoxInfo("No JSON syntax errors for this file",
                   "No JSON syntax errors (at or below the current logger_level) for " + fname);
        return;
    }
    auto lintArr = std::make_shared<JArray>();
    for (const JsonLint& lint : lints) lintArr->children.push_back(lint.ToJson());
    PrettyPrintJsonInNewFile(lintArr);
}

@end

static JTErrorForm* sErrorForm = nil;

static void errorFormEnsurePanel() {
    if (!sErrorForm) sErrorForm = [[JTErrorForm alloc] init];
    if (sErrorForm.panelHandle == 0) {
        tTbData tb = {};
        tb.hClient = (__bridge void*)sErrorForm.containerView;
        tb.pszName = "JSON errors";
        tb.dlgID = JT_ERROR_FORM_FUNC_INDEX;
        tb.uMask = DWS_DF_CONT_BOTTOM;
        tb.pszModuleName = "JsonTools";
        intptr_t h = Npp(NPPM_DMMREGASDCKDLG, 0, (intptr_t)&tb);
        if (h > 0) sErrorForm.panelHandle = (uint64_t)h;
    }
}

void jtErrorFormShow(const std::string& fname, const std::vector<JsonLint>& lints) {
    errorFormEnsurePanel();
    if (!sErrorForm || sErrorForm.panelHandle == 0) return;
    [sErrorForm reloadWithFname:fname lints:lints];
    Npp(NPPM_DMM_SHOWPANEL, (uintptr_t)sErrorForm.panelHandle, 0);
}

void jtErrorFormReload(const std::string& fname, const std::vector<JsonLint>& lints) {
    if (!sErrorForm) return;
    [sErrorForm reloadWithFname:fname lints:lints];
}

void jtErrorFormHide(void) {
    if (sErrorForm && sErrorForm.panelHandle != 0)
        Npp(NPPM_DMM_HIDEPANEL, (uintptr_t)sErrorForm.panelHandle, 0);
}

bool jtErrorFormIsVisible(void) {
    return sErrorForm && sErrorForm.containerView.window != nil &&
           !sErrorForm.containerView.hiddenOrHasHiddenAncestor;
}

void jtErrorFormApplyStyling(void) {
    if (sErrorForm) [sErrorForm.table reloadData];
}

void jtErrorFormShutdown(void) {
    if (sErrorForm && sErrorForm.panelHandle != 0) {
        Npp(NPPM_DMM_UNREGISTERPANEL, (uintptr_t)sErrorForm.panelHandle, 0);
        sErrorForm.panelHandle = 0;
    }
    sErrorForm = nil;
}

// ── sort form ────────────────────────────────────────────────────────────────
@interface JTSortForm : NSObject
@property(nonatomic, strong) NSPanel* panel;
@property(nonatomic, strong) NSTextField* pathBox;
@property(nonatomic, strong) NSPopUpButton* sortMethodBox;
@property(nonatomic, strong) NSTextField* queryKeyIndexBox;
@property(nonatomic, strong) NSButton* reverseCheck;
@property(nonatomic, strong) NSButton* multipleArraysCheck;
@end

@implementation JTSortForm {
    RemesParser _remesParser;
}

- (instancetype)init {
    if (!(self = [super init])) return nil;
    NSPanel* panel = [[NSPanel alloc]
        initWithContentRect:NSMakeRect(0, 0, 420, 240)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskUtilityWindow
                    backing:NSBackingStoreBuffered
                      defer:NO];
    panel.title = @"Sort arrays";
    panel.floatingPanel = YES;
    NSView* c = panel.contentView;
    auto label = [&](NSString* text, CGFloat x, CGFloat y, CGFloat w, CGFloat unused) {
        (void)unused;
        NSTextField* l = [NSTextField labelWithString:text];
        l.frame = NSMakeRect(x, y, w <= 0 ? 280 : w, 18);
        l.font = [NSFont systemFontOfSize:11];
        [c addSubview:l];
    };
    label(@"Path to array(s) (RemesPath)", 12, 206, 250, 0);
    _pathBox = [[NSTextField alloc] initWithFrame:NSMakeRect(12, 182, 396, 22)];
    _pathBox.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    [c addSubview:_pathBox];
    label(@"Sort method", 12, 156, 150, 0);
    _sortMethodBox = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(160, 150, 248, 24)];
    [_sortMethodBox addItemsWithTitles:@[
        @"Default", @"As strings (ignoring case)", @"By index/key of children",
        @"By query on children", @"Shuffle"
    ]];
    _sortMethodBox.target = self;
    _sortMethodBox.action = @selector(sortMethodChanged:);
    [c addSubview:_sortMethodBox];
    label(@"Key/index/query", 12, 124, 150, 0);
    _queryKeyIndexBox = [[NSTextField alloc] initWithFrame:NSMakeRect(160, 120, 248, 22)];
    _queryKeyIndexBox.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    _queryKeyIndexBox.enabled = NO;
    [c addSubview:_queryKeyIndexBox];
    _reverseCheck = [NSButton checkboxWithTitle:@"Reverse order (biggest first)"
                                         target:nil
                                         action:nil];
    _reverseCheck.frame = NSMakeRect(12, 88, 300, 20);
    [c addSubview:_reverseCheck];
    _multipleArraysCheck =
        [NSButton checkboxWithTitle:@"Sort each array inside the JSON at this path"
                             target:nil
                             action:nil];
    _multipleArraysCheck.frame = NSMakeRect(12, 62, 396, 20);
    [c addSubview:_multipleArraysCheck];
    NSButton* sortBtn = [NSButton buttonWithTitle:@"Sort" target:self action:@selector(sortClicked:)];
    sortBtn.frame = NSMakeRect(12, 16, 100, 30);
    sortBtn.keyEquivalent = @"\r";
    [c addSubview:sortBtn];
    _panel = panel;
    return self;
}

- (void)sortMethodChanged:(id)sender {
    NSInteger idx = _sortMethodBox.indexOfSelectedItem;
    _queryKeyIndexBox.enabled = idx == 2 || idx == 3;
    _reverseCheck.enabled = idx != 4;
}

- (bool)sortSingleJson:(const JNodePtr&)toSort
                 query:(const std::function<JNodePtr(JNodePtr)>&)query {
    JArray* arr = AsArr(toSort);
    if (!arr) {
        MsgBoxError("Can only sort arrays",
                    "Can only sort arrays, got JSON of type " + FormatDtype(toSort->type));
        return false;
    }
    try {
        switch (_sortMethodBox.indexOfSelectedItem) {
            case 0:
                std::stable_sort(arr->children.begin(), arr->children.end(),
                                 [](const JNodePtr& a, const JNodePtr& b) {
                                     return a->CompareTo(*b) < 0;
                                 });
                break;
            case 1:
                std::stable_sort(arr->children.begin(), arr->children.end(),
                                 [](const JNodePtr& a, const JNodePtr& b) {
                                     return StrToLower(a->ToString()) < StrToLower(b->ToString());
                                 });
                break;
            case 2:
            case 3:
                std::stable_sort(arr->children.begin(), arr->children.end(),
                                 [&query](const JNodePtr& a, const JNodePtr& b) {
                                     return query(a)->CompareTo(*query(b)) < 0;
                                 });
                break;
            case 4: {
                static std::mt19937_64 rng(std::random_device{}());
                std::shuffle(arr->children.begin(), arr->children.end(), rng);
                break;
            }
        }
    } catch (const std::exception& ex) {
        MsgBoxError("Error while sorting array", std::string("While sorting an array, got the "
                                                             "following error:\n") +
                                                     RemesParser::PrettifyException(ex));
        return false;
    }
    if (_reverseCheck.state == NSControlStateValueOn && _reverseCheck.enabled)
        std::reverse(arr->children.begin(), arr->children.end());
    return true;
}

- (void)sortClicked:(id)sender {
    ParseResult pr = TryParseJson(DocumentType::JSON, false, true);
    if (pr.parserState == ParserState::FATAL || !pr.node) return;
    std::string pathQuery = "@" + std::string(_pathBox.stringValue.UTF8String ?: "");
    JNodePtr jsonAtPath;
    try {
        jsonAtPath = _remesParser.Search(pathQuery, pr.node);
    } catch (const std::exception& ex) {
        MsgBoxError("Could not find json at that path",
                    "Could not find json at the specified path (" + pathQuery +
                        ").\nGot the following error:\n" + RemesParser::PrettifyException(ex));
        return;
    }
    NSInteger methodIdx = _sortMethodBox.indexOfSelectedItem;
    std::function<JNodePtr(JNodePtr)> query;
    if (methodIdx == 2 || methodIdx == 3) {
        std::string queryText = methodIdx == 2
            ? "@[" + std::string(_queryKeyIndexBox.stringValue.UTF8String ?: "") + "]"
            : std::string(_queryKeyIndexBox.stringValue.UTF8String ?: "");
        try {
            JNodePtr compiled = _remesParser.Compile(queryText);
            auto* cur = dynamic_cast<CurJson*>(compiled.get());
            if (!cur) throw JsonToolsError("sort query must be a function of the input");
            auto fn = cur->function;
            query = [fn](JNodePtr x) { return fn(x); };
        } catch (const std::exception& ex) {
            MsgBoxError("Failed to compile query for sorting",
                        "Based on selected sort method, attempted to compile query \"" +
                            std::string(_queryKeyIndexBox.stringValue.UTF8String ?: "") +
                            "\",\nbut got the following error:\n" +
                            RemesParser::PrettifyException(ex));
            return;
        }
    }
    if (_multipleArraysCheck.state == NSControlStateValueOn) {
        if (JArray* arr = AsArr(jsonAtPath)) {
            for (JNodePtr& child : arr->children)
                if (![self sortSingleJson:child query:query]) return;
        } else if (JObject* obj = AsObj(jsonAtPath)) {
            for (auto& kv : obj->children)
                if (![self sortSingleJson:kv.second query:query]) return;
        } else {
            MsgBoxError("JSON at specified path must be object or array",
                        "JSON at the specified path must be object or array, got type " +
                            FormatDtype(jsonAtPath->type));
            return;
        }
    } else {
        if (![self sortSingleJson:jsonAtPath query:query]) return;
    }
    std::function<std::string(const JNodePtr&)> formatter = PrettyPrintFromSettings;
    if (pr.documentType == DocumentType::JSONL) formatter = ToJsonLinesFromSettings;
    ReformatFileWithJson(pr.node, formatter, pr.usesSelections);
}

@end

static JTSortForm* sSortForm = nil;

void jtShowSortForm(const std::string& pathText) {
    if (!sSortForm) sSortForm = [[JTSortForm alloc] init];
    sSortForm.pathBox.stringValue = U8(pathText);
    [sSortForm.panel makeKeyAndOrderFront:nil];
}

void jtSortFormApplyStyling(void) {}

// ── JSON-to-CSV dialog ───────────────────────────────────────────────────────
void jtShowJsonToCsvDialog(JNodePtr json) {
    NSWindow* window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 380, 250)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    window.title = @"JSON to CSV";
    NSView* c = window.contentView;
    auto label = [&](NSString* text, CGFloat y) {
        NSTextField* l = [NSTextField labelWithString:text];
        l.frame = NSMakeRect(12, y, 170, 18);
        l.font = [NSFont systemFontOfSize:11];
        [c addSubview:l];
    };
    label(@"Key separator", 212);
    NSTextField* keySepBox = [[NSTextField alloc] initWithFrame:NSMakeRect(190, 208, 170, 22)];
    keySepBox.stringValue = @".";
    [c addSubview:keySepBox];
    label(@"Delimiter in output file", 182);
    NSPopUpButton* delimBox = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(188, 176, 174, 24)];
    [delimBox addItemsWithTitles:@[ @",", @"Tab (\\t)", @"|" ]];
    [c addSubview:delimBox];
    label(@"Strategy", 152);
    NSPopUpButton* strategyBox = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(188, 146, 174, 24)];
    [strategyBox
        addItemsWithTitles:@[ @"Default", @"Full recursive", @"No recursion", @"Stringify iterables" ]];
    [c addSubview:strategyBox];
    label(@"Line terminator", 122);
    NSPopUpButton* eolBox = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(188, 116, 174, 24)];
    [eolBox addItemsWithTitles:@[ @"CRLF (\\r\\n)", @"CR (\\r)", @"LF (\\n)" ]];
    [eolBox selectItemAtIndex:(NSInteger)jtSettings().csv_newline];
    [c addSubview:eolBox];
    NSButton* boolsAsInts = [NSButton checkboxWithTitle:@"Convert true/false to 1/0"
                                                 target:nil
                                                 action:nil];
    boolsAsInts.frame = NSMakeRect(12, 86, 300, 20);
    [c addSubview:boolsAsInts];
    NSButton* genBtn = [NSButton buttonWithTitle:@"Generate CSV" target:NSApp
                                          action:@selector(stopModal)];
    genBtn.keyEquivalent = @"\r";
    genBtn.frame = NSMakeRect(220, 16, 140, 30);
    [c addSubview:genBtn];
    NSButton* cancelBtn = [NSButton buttonWithTitle:@"Cancel" target:NSApp
                                             action:@selector(abortModal)];
    cancelBtn.keyEquivalent = @"\033";
    cancelBtn.frame = NSMakeRect(118, 16, 100, 30);
    [c addSubview:cancelBtn];
    [window center];
    NSModalResponse resp = [NSApp runModalForWindow:window];
    [window orderOut:nil];
    if (resp != NSModalResponseStop) return;
    std::string keysepText = keySepBox.stringValue.UTF8String ?: ".";
    std::string keysep = keysepText.empty() ? "." : std::string(1, keysepText[0]);
    char delim = ',';
    switch (delimBox.indexOfSelectedItem) {
        case 0: delim = ','; break;
        case 1: delim = '\t'; break;
        case 2: delim = '|'; break;
    }
    JsonTabularizer tabularizer;
    switch (strategyBox.indexOfSelectedItem) {
        case 0: tabularizer.strategy = JsonTabularizerStrategy::DEFAULT; break;
        case 1: tabularizer.strategy = JsonTabularizerStrategy::FULL_RECURSIVE; break;
        case 2: tabularizer.strategy = JsonTabularizerStrategy::NO_RECURSION; break;
        case 3: tabularizer.strategy = JsonTabularizerStrategy::STRINGIFY_ITERABLES; break;
    }
    std::string eol = "\r\n";
    switch (eolBox.indexOfSelectedItem) {
        case 0: eol = "\r\n"; break;
        case 1: eol = "\r"; break;
        case 2: eol = "\n"; break;
    }
    std::string csv;
    try {
        JNodePtr schema = JsonSchemaMaker::BuildSchema(json);
        auto tab = tabularizer.BuildTable(json, schema, keysep);
        csv = tabularizer.TableToCsv(tab.get(), delim, '"', eol, nullptr,
                                     boolsAsInts.state == NSControlStateValueOn);
    } catch (const std::exception& ex) {
        MsgBoxError("Exception while converting JSON to CSV",
                    std::string("While trying to create CSV from JSON, raised this exception:\n") +
                        RemesParser::PrettifyException(ex));
        return;
    }
    FileNew();
    AppendText(csv);
    // the host may not emit NPPN_BUFFERACTIVATED synchronously after FileNew
}

// ── regex search form ────────────────────────────────────────────────────────
@interface JTRegexSearchForm : NSObject
@property(nonatomic, strong) NSPanel* panel;
@property(nonatomic, strong) NSTextField* regexBox;
@property(nonatomic, strong) NSButton* ignoreCaseCheck;
@property(nonatomic, strong) NSButton* fullMatchCheck;
@property(nonatomic, strong) NSButton* parseAsCsvCheck;
@property(nonatomic, strong) NSTextField* delimiterBox;
@property(nonatomic, strong) NSTextField* quoteCharBox;
@property(nonatomic, strong) NSPopUpButton* newlineBox;
@property(nonatomic, strong) NSTextField* nColumnsBox;
@property(nonatomic, strong) NSPopUpButton* headerHandlingBox;
@property(nonatomic, strong) NSTextField* numColsBox;
@end

@implementation JTRegexSearchForm

- (instancetype)init {
    if (!(self = [super init])) return nil;
    NSPanel* panel = [[NSPanel alloc]
        initWithContentRect:NSMakeRect(0, 0, 430, 330)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskUtilityWindow
                    backing:NSBackingStoreBuffered
                      defer:NO];
    panel.title = @"Regex search to JSON";
    panel.floatingPanel = YES;
    NSView* c = panel.contentView;
    auto label = [&](NSString* text, CGFloat x, CGFloat y, CGFloat w, CGFloat unused) {
        (void)unused;
        NSTextField* l = [NSTextField labelWithString:text];
        l.frame = NSMakeRect(x, y, w <= 0 ? 280 : w, 18);
        l.font = [NSFont systemFontOfSize:11];
        [c addSubview:l];
        return l;
    };
    label(@"Regex", 12, 296, 100, 0);
    _regexBox = [[NSTextField alloc] initWithFrame:NSMakeRect(120, 292, 296, 22)];
    _regexBox.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    [c addSubview:_regexBox];
    _ignoreCaseCheck = [NSButton checkboxWithTitle:@"Ignore case" target:nil action:nil];
    _ignoreCaseCheck.frame = NSMakeRect(12, 264, 160, 20);
    [c addSubview:_ignoreCaseCheck];
    _fullMatchCheck = [NSButton checkboxWithTitle:@"Include full match as first item"
                                           target:nil
                                           action:nil];
    _fullMatchCheck.frame = NSMakeRect(180, 264, 240, 20);
    [c addSubview:_fullMatchCheck];
    _parseAsCsvCheck = [NSButton checkboxWithTitle:@"Parse as CSV?"
                                            target:self
                                            action:@selector(csvToggled:)];
    _parseAsCsvCheck.frame = NSMakeRect(12, 234, 200, 20);
    [c addSubview:_parseAsCsvCheck];
    label(@"Delimiter", 12, 206, 100, 0);
    _delimiterBox = [[NSTextField alloc] initWithFrame:NSMakeRect(120, 202, 80, 22)];
    _delimiterBox.stringValue = @",";
    [c addSubview:_delimiterBox];
    label(@"Quote character", 12, 178, 104, 0);
    _quoteCharBox = [[NSTextField alloc] initWithFrame:NSMakeRect(120, 174, 80, 22)];
    _quoteCharBox.stringValue = @"\"";
    [c addSubview:_quoteCharBox];
    label(@"Newline", 12, 148, 100, 0);
    _newlineBox = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(118, 142, 120, 24)];
    [_newlineBox addItemsWithTitles:@[ @"CRLF (\\r\\n)", @"LF (\\n)", @"CR (\\r)" ]];
    [c addSubview:_newlineBox];
    label(@"Number of columns", 12, 118, 130, 0);
    _nColumnsBox = [[NSTextField alloc] initWithFrame:NSMakeRect(150, 114, 80, 22)];
    [c addSubview:_nColumnsBox];
    label(@"Header handling", 12, 88, 120, 0);
    _headerHandlingBox = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(148, 82, 200, 24)];
    [_headerHandlingBox
        addItemsWithTitles:@[ @"Skip header row", @"Include header row", @"Use header as keys" ]];
    [c addSubview:_headerHandlingBox];
    label(@"Columns to parse as numbers (JSON array)", 12, 58, 280, 0);
    _numColsBox = [[NSTextField alloc] initWithFrame:NSMakeRect(296, 54, 120, 22)];
    [c addSubview:_numColsBox];
    NSButton* searchBtn = [NSButton buttonWithTitle:@"Search"
                                             target:self
                                             action:@selector(searchClicked:)];
    searchBtn.frame = NSMakeRect(12, 12, 110, 30);
    searchBtn.keyEquivalent = @"\r";
    [c addSubview:searchBtn];
    _panel = panel;
    [self setCsvControlsEnabled:false];
    return self;
}

- (void)setCsvControlsEnabled:(bool)showCsv {
    _delimiterBox.enabled = showCsv;
    _quoteCharBox.enabled = showCsv;
    _newlineBox.enabled = showCsv;
    _nColumnsBox.enabled = showCsv;
    _headerHandlingBox.enabled = showCsv;
    _regexBox.enabled = !showCsv;
    _ignoreCaseCheck.enabled = !showCsv;
    _fullMatchCheck.enabled = !showCsv;
}

- (void)csvToggled:(id)sender {
    bool showCsv = _parseAsCsvCheck.state == NSControlStateValueOn;
    [self setCsvControlsEnabled:showCsv];
    if (showCsv && jtSettings().auto_try_guess_csv_delim_newline) [self trySniff];
}

- (void)trySniff {
    EndOfLine eol;
    char delim;
    int64_t nColumns;
    if ([JTRegexSearchForm trySniffCommonDelimsAndEols:&eol delim:&delim nColumns:&nColumns]) {
        [self setCsvSettingsChecked:true eol:eol delim:delim nColumns:nColumns];
    }
}

+ (bool)trySniffCommonDelimsAndEols:(EndOfLine*)eol delim:(char*)delim nColumns:(int64_t*)nColumns {
    *eol = EndOfLine::CRLF;
    *delim = '\0';
    *nColumns = -1;
    int64_t sniffLen = CsvSniffer::DEFAULT_MAX_CHARS_TO_SNIFF * 12 / 10;
    int64_t len = DocLength();
    std::string text = GetSlice(0, len < sniffLen ? len : sniffLen);
    int64_t crlfCount = 0, crCount = 0, lfCount = 0;
    for (size_t ii = 0; ii < text.size();) {
        char ch = text[ii++];
        if (ch == '\r') {
            if (ii < text.size() && text[ii] == '\n') {
                crlfCount++;
                ii++;
            } else {
                crCount++;
            }
        } else if (ch == '\n') {
            lfCount++;
        }
    }
    EndOfLine maybeEol = EndOfLine::CRLF;
    if (crCount > crlfCount && crCount > lfCount)
        maybeEol = EndOfLine::CR;
    else if (lfCount > crCount && lfCount > crlfCount)
        maybeEol = EndOfLine::LF;
    for (char maybeDelim : {',', '\t'}) {
        int64_t cols = CsvSniffer::Sniff(text, maybeEol, maybeDelim, '"');
        if (cols >= 2) {
            *delim = maybeDelim;
            *eol = maybeEol;
            *nColumns = cols;
            return true;
        }
    }
    return false;
}

- (void)setCsvSettingsChecked:(bool)checked eol:(EndOfLine)eol delim:(char)delim
                     nColumns:(int64_t)nColumns {
    _parseAsCsvCheck.state = checked ? NSControlStateValueOn : NSControlStateValueOff;
    [self setCsvControlsEnabled:checked];
    if (checked) {
        _nColumnsBox.stringValue = [NSString stringWithFormat:@"%lld", (long long)nColumns];
        _delimiterBox.stringValue = delim == '\t' ? @"\\t" : [NSString stringWithFormat:@"%c", delim];
        _quoteCharBox.stringValue = @"\"";
        [_newlineBox selectItemAtIndex:eol == EndOfLine::CRLF ? 0 : eol == EndOfLine::LF ? 1 : 2];
    }
}

- (void)getTreeViewInRegexMode {
    if (openTreeViewer == nil || [openTreeViewer fname] != activeFname) {
        CmdOpenJsonTree(DocumentType::REGEX);
    } else {
        [openTreeViewer setDocumentTypeComboBoxIndex:DocumentType::REGEX];
    }
}

- (void)searchClicked:(id)sender {
    [self getTreeViewInRegexMode];
    if (!openTreeViewer) return;
    std::string columnsToParseAsNumber;
    std::string numColsText = _numColsBox.stringValue.UTF8String ?: "";
    if (!numColsText.empty()) {
        try {
            JsonParser parser(LoggerLevel::JSON5, false, false, false);
            JNodePtr arrNode = parser.Parse(numColsText);
            JArray* arr = AsArr(arrNode);
            bool ok = !parser.fatal() && arr && arr->Length() > 0;
            if (ok)
                for (JNodePtr& x : arr->children)
                    if (x->type != DT_INT) ok = false;
            if (!ok) {
                MsgBoxError("Columns to parse as number must be array of integers",
                            "Columns to parse as number must be a nonempty JSON array of integers");
            } else {
                std::string s = arrNode->ToString(false);
                columnsToParseAsNumber = ", " + s.substr(1, s.size() - 2);
            }
        } catch (...) {
        }
    }
    std::string query;
    if (_parseAsCsvCheck.state == NSControlStateValueOn) {
        std::string delimText = _delimiterBox.stringValue.UTF8String ?: ",";
        std::string delim = delimText == "\\t"
            ? "`\\t`" : RemesPathLexer::StringToBacktickString(delimText);
        std::string quote =
            RemesPathLexer::StringToBacktickString(_quoteCharBox.stringValue.UTF8String ?: "\"");
        static const char* EOLS[] = {"`\\r\\n`", "`\\n`", "`\\r`"};
        NSInteger eolIdx = _newlineBox.indexOfSelectedItem;
        std::string newline = EOLS[(eolIdx >= 0 && eolIdx < 3) ? eolIdx : 0];
        static const char HEADER_ABBREVS[] = {'n', 'h', 'd'};
        NSInteger hIdx = _headerHandlingBox.indexOfSelectedItem;
        char headerAbbrev = HEADER_ABBREVS[(hIdx >= 0 && hIdx < 3) ? hIdx : 0];
        query = "s_csv(@, " + std::string(_nColumnsBox.stringValue.UTF8String ?: "1") + ", " +
                delim + ", " + newline + ", " + quote + ", " + headerAbbrev +
                columnsToParseAsNumber + ")";
    } else {
        std::string ignoreCaseFlag =
            _ignoreCaseCheck.state == NSControlStateValueOn ? "(?i)" : "(?-i)";
        std::string regex = RemesPathLexer::StringToBacktickString(
            ignoreCaseFlag + std::string(_regexBox.stringValue.UTF8String ?: ""));
        std::string includeFullMatch =
            _fullMatchCheck.state == NSControlStateValueOn ? "true" : "false";
        query = "s_fa(@, " + regex + ", " + includeFullMatch + columnsToParseAsNumber + ")";
    }
    [openTreeViewer setQueryBoxText:U8(query)];
    if (![openTreeViewer isVisibleOnScreen]) [openTreeViewer showPanel];
    [openTreeViewer submitQuery];
}

@end

static JTRegexSearchForm* sRegexSearchForm = nil;

void jtShowRegexSearchForm(void) {
    bool isNew = sRegexSearchForm == nil;
    if (!sRegexSearchForm) sRegexSearchForm = [[JTRegexSearchForm alloc] init];
    [sRegexSearchForm getTreeViewInRegexMode];
    [sRegexSearchForm.panel makeKeyAndOrderFront:nil];
    [sRegexSearchForm.panel makeFirstResponder:sRegexSearchForm.regexBox];
    if (isNew && jtSettings().auto_try_guess_csv_delim_newline) [sRegexSearchForm trySniff];
}

void jtRegexSearchFormApplyStyling(void) {}

// ── find/replace form ────────────────────────────────────────────────────────
// C#'s Regex.Escape
static std::string RegexEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '\\': case '*': case '+': case '?': case '|': case '{': case '[':
            case '(': case ')': case '^': case '$': case '.': case '#':
                out += '\\';
                out += c;
                break;
            case ' ': out += "\\ "; break;
            case '\t': out += "\\t"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\f': out += "\\f"; break;
            default: out += c; break;
        }
    }
    return out;
}

static std::string ReplaceAllStr(std::string s, const std::string& from, const std::string& to) {
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

@interface JTFindReplaceForm : NSObject
@property(nonatomic, strong) NSPanel* panel;
@property(nonatomic, weak) JTTreeViewer* treeViewer;
@property(nonatomic, strong) NSTextField* findBox;
@property(nonatomic, strong) NSTextField* replaceBox;
@property(nonatomic, strong) NSTextField* rootBox;
@property(nonatomic, strong) NSButton* recursiveCheck;
@property(nonatomic, strong) NSPopUpButton* keysValsBothBox;
@property(nonatomic, strong) NSButton* regexCheck;
@property(nonatomic, strong) NSButton* matchExactlyCheck;
@property(nonatomic, strong) NSButton* ignoreCaseCheck;
@property(nonatomic, strong) NSButton* mathCheck;
@end

@implementation JTFindReplaceForm {
    std::string _findQuery;
    std::string _replaceQuery;
    NSInteger _previousKeysValsBothIndex;
}

- (instancetype)initWithTreeViewer:(JTTreeViewer*)treeViewer {
    if (!(self = [super init])) return nil;
    _treeViewer = treeViewer;
    _previousKeysValsBothIndex = 2;
    NSPanel* panel = [[NSPanel alloc]
        initWithContentRect:NSMakeRect(0, 0, 400, 330)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskUtilityWindow
                    backing:NSBackingStoreBuffered
                      defer:NO];
    panel.title = @"Find/replace in JSON";
    panel.floatingPanel = YES;
    NSView* c = panel.contentView;
    auto label = [&](NSString* text, CGFloat x, CGFloat y) {
        NSTextField* l = [NSTextField labelWithString:text];
        l.frame = NSMakeRect(x, y, 130, 18);
        l.font = [NSFont systemFontOfSize:11];
        [c addSubview:l];
    };
    label(@"Find...", 12, 296);
    _findBox = [[NSTextField alloc] initWithFrame:NSMakeRect(140, 292, 246, 22)];
    [c addSubview:_findBox];
    label(@"...replace with", 12, 266);
    _replaceBox = [[NSTextField alloc] initWithFrame:NSMakeRect(140, 262, 246, 22)];
    [c addSubview:_replaceBox];
    NSButton* swapBtn = [NSButton buttonWithTitle:@"Swap find/replace"
                                           target:self
                                           action:@selector(swapClicked:)];
    swapBtn.frame = NSMakeRect(12, 228, 170, 26);
    [c addSubview:swapBtn];
    label(@"Root (RemesPath)", 12, 202);
    _rootBox = [[NSTextField alloc] initWithFrame:NSMakeRect(140, 198, 246, 22)];
    [c addSubview:_rootBox];
    _recursiveCheck = [NSButton checkboxWithTitle:@"Recursive search?" target:nil action:nil];
    _recursiveCheck.frame = NSMakeRect(12, 170, 180, 20);
    _recursiveCheck.state = NSControlStateValueOn;
    [c addSubview:_recursiveCheck];
    label(@"Search in keys or values?", 12, 144);
    _keysValsBothBox = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(186, 138, 200, 24)];
    [_keysValsBothBox addItemsWithTitles:@[ @"Keys", @"Values", @"Keys and values" ]];
    [_keysValsBothBox selectItemAtIndex:2];
    [c addSubview:_keysValsBothBox];
    _regexCheck = [NSButton checkboxWithTitle:@"Use regular expressions?"
                                       target:self
                                       action:@selector(regexToggled:)];
    _regexCheck.frame = NSMakeRect(12, 112, 220, 20);
    [c addSubview:_regexCheck];
    _matchExactlyCheck = [NSButton checkboxWithTitle:@"Match exactly?"
                                              target:self
                                              action:@selector(matchExactlyToggled:)];
    _matchExactlyCheck.frame = NSMakeRect(12, 88, 220, 20);
    [c addSubview:_matchExactlyCheck];
    _ignoreCaseCheck = [NSButton checkboxWithTitle:@"Ignore case?" target:nil action:nil];
    _ignoreCaseCheck.frame = NSMakeRect(12, 64, 220, 20);
    [c addSubview:_ignoreCaseCheck];
    _mathCheck = [NSButton checkboxWithTitle:@"Math expression?"
                                      target:self
                                      action:@selector(mathToggled:)];
    _mathCheck.frame = NSMakeRect(232, 64, 160, 20);
    [c addSubview:_mathCheck];
    NSButton* findBtn = [NSButton buttonWithTitle:@"Find all"
                                           target:self
                                           action:@selector(findClicked:)];
    findBtn.frame = NSMakeRect(12, 16, 120, 30);
    [c addSubview:findBtn];
    NSButton* replaceBtn = [NSButton buttonWithTitle:@"Replace all"
                                              target:self
                                              action:@selector(replaceClicked:)];
    replaceBtn.frame = NSMakeRect(146, 16, 120, 30);
    [c addSubview:replaceBtn];
    _panel = panel;
    return self;
}

- (void)swapClicked:(id)sender {
    NSString* tmp = _findBox.stringValue;
    _findBox.stringValue = _replaceBox.stringValue;
    _replaceBox.stringValue = tmp;
}

- (void)regexToggled:(id)sender {
    if (_regexCheck.state == NSControlStateValueOn)
        _matchExactlyCheck.state = NSControlStateValueOff;
    _matchExactlyCheck.enabled = _regexCheck.state != NSControlStateValueOn;
}

- (void)matchExactlyToggled:(id)sender {
    if (_matchExactlyCheck.state == NSControlStateValueOn)
        _regexCheck.state = NSControlStateValueOff;
    _regexCheck.enabled = _matchExactlyCheck.state != NSControlStateValueOn;
}

- (void)mathToggled:(id)sender {
    if (_mathCheck.state == NSControlStateValueOn) {
        _previousKeysValsBothIndex = _keysValsBothBox.indexOfSelectedItem;
        [_keysValsBothBox selectItemAtIndex:1];
        _keysValsBothBox.enabled = NO;
        return;
    }
    [_keysValsBothBox selectItemAtIndex:_previousKeysValsBothIndex];
    _keysValsBothBox.enabled = YES;
}

- (void)assignFindReplaceQueries {
    std::string rootText = _rootBox.stringValue.UTF8String ?: "";
    std::string root = "@" + rootText;
    std::string findText = _findBox.stringValue.UTF8String ?: "";
    std::string replaceText = _replaceBox.stringValue.UTF8String ?: "";
    bool recursive = _recursiveCheck.state == NSControlStateValueOn;
    bool ignoreCase = _ignoreCaseCheck.state == NSControlStateValueOn;
    if (_mathCheck.state == NSControlStateValueOn) {
        static Regex binopRegex("[\\+\\-\\|&^%\\*/]|\\*\\*|//|[<>]=?|[=!]=");
        _replaceQuery = binopRegex.IsMatch(replaceText) ? "@ " + replaceText : replaceText;
        if (recursive)
            _findQuery = "(" + root + ")..*[is_num(@)][@ " + findText + "]";
        else
            _findQuery = "(" + root + ").*[is_num(@)][@ " + findText + "]";
        return;
    }
    std::string keysFindText, valuesFindText;
    std::string escapedBacktick = ReplaceAllStr(findText, "`", "\\`");
    if (_regexCheck.state == NSControlStateValueOn) {
        keysFindText = (ignoreCase ? "g`(?i)" : "g`") + escapedBacktick + "`";
        valuesFindText = "[str(@) =~ " + keysFindText + "]";
    } else if (_matchExactlyCheck.state == NSControlStateValueOn) {
        if (ignoreCase) {
            keysFindText = "g`(?i)\\A" +
                           ReplaceAllStr(ReplaceAllStr(RegexEscape(findText), "\\", "\\\\"), "`",
                                         "\\`") +
                           "\\z`";
            valuesFindText = "[str(@) =~ " + keysFindText + "]";
        } else {
            keysFindText = "`" + escapedBacktick + "`";
            valuesFindText = "[str(@) == " + keysFindText + "]";
        }
    } else {
        std::string escaped =
            ReplaceAllStr(ReplaceAllStr(RegexEscape(findText), "\\", "\\\\"), "`", "\\`");
        keysFindText = (ignoreCase ? "g`(?i)" : "g`") + escaped + "`";
        valuesFindText = "[str(@) =~ " + keysFindText + "]";
    }
    _replaceQuery =
        "s_sub(@, " + keysFindText + ", `" + ReplaceAllStr(replaceText, "`", "\\`") + "`)";
    std::string dotOp = recursive ? ".." : ".";
    std::string starOp = recursive ? "..*" : ".*";
    switch (_keysValsBothBox.indexOfSelectedItem) {
        case 0: _findQuery = "(" + root + ")" + dotOp + keysFindText; break;
        case 1:
            _findQuery = recursive ? "(" + root + ")..*" + valuesFindText
                                   : root + valuesFindText;
            break;
        default:
            _findQuery = "concat(\r\n    (" + root + ")" + dotOp + keysFindText + ",\r\n    " +
                         (recursive ? "(" + root + ")..*" : root) + valuesFindText + "\r\n)";
            break;
    }
}

- (void)findClicked:(id)sender {
    JTTreeViewer* tv = _treeViewer;
    if (!tv) return;
    [tv refreshClicked];
    [self assignFindReplaceQueries];
    [tv setQueryBoxText:U8(_findQuery)];
    [tv submitQuery];
    [tv expandRootNode];
}

- (void)replaceClicked:(id)sender {
    JTTreeViewer* tv = _treeViewer;
    if (!tv) return;
    NSInteger keysvalsIndex = _keysValsBothBox.indexOfSelectedItem;
    [_keysValsBothBox selectItemAtIndex:1];
    [self assignFindReplaceQueries];
    std::string rootText = _rootBox.stringValue.UTF8String ?: "";
    bool recursive = _recursiveCheck.state == NSControlStateValueOn;
    std::string root = recursive ? "(@" + rootText + ")..*" : "@" + rootText;
    std::string findPart = _mathCheck.state == NSControlStateValueOn
        ? "var x = " + _findQuery
        : "var x = " + root + "[is_str(@)]";
    std::string query = findPart + ";\r\nx = " + _replaceQuery + ";\r\nx";
    [tv setQueryBoxText:U8(query)];
    [tv submitQuery];
    [tv expandRootNode];
    [_keysValsBothBox selectItemAtIndex:keysvalsIndex];
}

@end

static JTFindReplaceForm* sFindReplaceForm = nil;

void jtShowFindReplaceForm(JTTreeViewer* treeViewer) {
    if (!sFindReplaceForm || sFindReplaceForm.treeViewer != treeViewer) {
        if (sFindReplaceForm) [sFindReplaceForm.panel close];
        sFindReplaceForm = [[JTFindReplaceForm alloc] initWithTreeViewer:treeViewer];
        // if the user is querying a subset of the JSON, find/replace is done on it
        // (upstream pre-fills from the selected tree node; the port starts at the root)
    }
    [sFindReplaceForm.panel makeKeyAndOrderFront:nil];
    [sFindReplaceForm.panel makeFirstResponder:sFindReplaceForm.findBox];
}

void jtCloseFindReplaceFormFor(JTTreeViewer* treeViewer) {
    if (sFindReplaceForm && sFindReplaceForm.treeViewer == treeViewer) {
        [sFindReplaceForm.panel close];
        sFindReplaceForm = nil;
    }
}
