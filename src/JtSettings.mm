// JsonTools.macos — JtSettings.mm: see JtSettings.h.

#include "JtSettings.h"

#include "JtNpp.h"

#include <fstream>
#include <map>
#include <sstream>

using namespace jsontools;

JtSettings& jtSettings() {
    static JtSettings s;
    return s;
}

static std::string iniPath() {
    std::string dir = jtnpp::ConfigSubDirectory();
    return dir.empty() ? "" : dir + "/JsonTools.ini";
}

static std::string trimWs(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

static bool parseBool(const std::string& v, bool fallback) {
    std::string t;
    for (char c : trimWs(v)) t += (char)tolower((unsigned char)c);
    if (t == "true" || t == "1") return true;
    if (t == "false" || t == "0") return false;
    return fallback;
}

static const char* LOGGER_LEVEL_NAMES[] = {"STRICT", "OK", "NAN_INF", "JSONC", "JSON5"};
static const char* PPSTYLE_NAMES[] = {"Google", "Whitesmith", "PPrint"};
static const char* KEYSTYLE_NAMES[] = {"JavaScript", "Python", "RemesPath"};
static const char* ASK_NAMES[] = {"DONT_DO_DONT_ASK", "ASK_BEFORE_DOING", "DO_WITHOUT_ASKING"};
static const char* EOL_NAMES[] = {"CRLF", "CR", "LF"};

template <typename E, size_t N>
static E parseEnum(const std::string& v, const char* (&names)[N], E fallback) {
    std::string t = trimWs(v);
    for (size_t i = 0; i < N; i++)
        if (t == names[i]) return static_cast<E>(i);
    return fallback;
}

void jtSettingsLoad() {
    std::string path = iniPath();
    if (path.empty()) return;
    std::ifstream in(path);
    if (!in) return;
    std::map<std::string, std::string> kv;
    std::string line;
    while (std::getline(in, line)) {
        line = trimWs(line);
        if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == '[') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        kv[trimWs(line.substr(0, eq))] = trimWs(line.substr(eq + 1));
    }
    JtSettings& s = jtSettings();
    auto has = [&](const char* k) { return kv.count(k) > 0; };
    auto str = [&](const char* k, const std::string& fb) { return has(k) ? kv[k] : fb; };
    auto getBool = [&](const char* k, bool fb) { return has(k) ? parseBool(kv[k], fb) : fb; };
    auto getInt = [&](const char* k, int fb) {
        if (!has(k)) return fb;
        try { return std::stoi(kv[k]); } catch (...) { return fb; }
    };
    auto getDouble = [&](const char* k, double fb) {
        if (!has(k)) return fb;
        try { return std::stod(kv[k]); } catch (...) { return fb; }
    };
    s.logger_level = parseEnum(str("logger_level", ""), LOGGER_LEVEL_NAMES, s.logger_level);
    s.offer_to_show_lint = getBool("offer_to_show_lint", s.offer_to_show_lint);
    s.max_file_size_MB_slow_actions =
        getDouble("max_file_size_MB_slow_actions", s.max_file_size_MB_slow_actions);
    s.auto_validate = getBool("auto_validate", s.auto_validate);
    s.inactivity_seconds_before_parse =
        getInt("inactivity_seconds_before_parse", s.inactivity_seconds_before_parse);
    s.max_json_length_full_tree = getInt("max_json_length_full_tree", s.max_json_length_full_tree);
    s.tree_node_images = getBool("tree_node_images", s.tree_node_images);
    s.indent_pretty_print = getInt("indent_pretty_print", s.indent_pretty_print);
    s.tab_indent_pretty_print = getBool("tab_indent_pretty_print", s.tab_indent_pretty_print);
    s.minimal_whitespace_compression =
        getBool("minimal_whitespace_compression", s.minimal_whitespace_compression);
    s.sort_keys = getBool("sort_keys", s.sort_keys);
    s.pretty_print_style = parseEnum(str("pretty_print_style", ""), PPSTYLE_NAMES, s.pretty_print_style);
    s.remember_comments = getBool("remember_comments", s.remember_comments);
    s.ask_before_pretty_printing_json_lines = parseEnum(
        str("ask_before_pretty_printing_json_lines", ""), ASK_NAMES, s.ask_before_pretty_printing_json_lines);
    s.key_style = parseEnum(str("key_style", ""), KEYSTYLE_NAMES, s.key_style);
    s.keyboard_shortcuts = getBool("keyboard_shortcuts", s.keyboard_shortcuts);
    s.path_separator = str("path_separator", s.path_separator);
    s.try_parse_start_chars = str("try_parse_start_chars", s.try_parse_start_chars);
    s.skip_api_request_and_fuzz_tests =
        getBool("skip_api_request_and_fuzz_tests", s.skip_api_request_and_fuzz_tests);
    s.csv_newline = parseEnum(str("csv_newline", ""), EOL_NAMES, s.csv_newline);
    s.toolbar_icons = str("toolbar_icons", s.toolbar_icons);
    s.auto_try_guess_csv_delim_newline =
        getBool("auto_try_guess_csv_delim_newline", s.auto_try_guess_csv_delim_newline);
    s.use_npp_styling = getBool("use_npp_styling", s.use_npp_styling);
    s.tree_view_font_size = (float)getDouble("tree_view_font_size", s.tree_view_font_size);
    s.minArrayLength = getInt("minArrayLength", s.minArrayLength);
    s.maxArrayLength = getInt("maxArrayLength", s.maxArrayLength);
    s.extended_ascii_strings = getBool("extended_ascii_strings", s.extended_ascii_strings);
    s.generate_random_patterns = getBool("generate_random_patterns", s.generate_random_patterns);
    s.max_schema_validation_problems =
        getInt("max_schema_validation_problems", s.max_schema_validation_problems);
}

void jtSettingsSave() {
    std::string path = iniPath();
    if (path.empty()) return;
    const JtSettings& s = jtSettings();
    auto b = [](bool v) { return v ? "True" : "False"; };
    std::ostringstream out;
    out << "; JsonTools settings (same names as the Windows plugin's ini)\n";
    out << "[main]\n";
    out << "logger_level=" << LOGGER_LEVEL_NAMES[(int)s.logger_level] << "\n";
    out << "offer_to_show_lint=" << b(s.offer_to_show_lint) << "\n";
    out << "max_file_size_MB_slow_actions=" << s.max_file_size_MB_slow_actions << "\n";
    out << "auto_validate=" << b(s.auto_validate) << "\n";
    out << "inactivity_seconds_before_parse=" << s.inactivity_seconds_before_parse << "\n";
    out << "max_json_length_full_tree=" << s.max_json_length_full_tree << "\n";
    out << "tree_node_images=" << b(s.tree_node_images) << "\n";
    out << "indent_pretty_print=" << s.indent_pretty_print << "\n";
    out << "tab_indent_pretty_print=" << b(s.tab_indent_pretty_print) << "\n";
    out << "minimal_whitespace_compression=" << b(s.minimal_whitespace_compression) << "\n";
    out << "sort_keys=" << b(s.sort_keys) << "\n";
    out << "pretty_print_style=" << PPSTYLE_NAMES[(int)s.pretty_print_style] << "\n";
    out << "remember_comments=" << b(s.remember_comments) << "\n";
    out << "ask_before_pretty_printing_json_lines="
        << ASK_NAMES[(int)s.ask_before_pretty_printing_json_lines] << "\n";
    out << "key_style=" << KEYSTYLE_NAMES[(int)s.key_style] << "\n";
    out << "keyboard_shortcuts=" << b(s.keyboard_shortcuts) << "\n";
    out << "path_separator=" << s.path_separator << "\n";
    out << "try_parse_start_chars=" << s.try_parse_start_chars << "\n";
    out << "skip_api_request_and_fuzz_tests=" << b(s.skip_api_request_and_fuzz_tests) << "\n";
    out << "csv_newline=" << EOL_NAMES[(int)s.csv_newline] << "\n";
    out << "toolbar_icons=" << s.toolbar_icons << "\n";
    out << "auto_try_guess_csv_delim_newline=" << b(s.auto_try_guess_csv_delim_newline) << "\n";
    out << "use_npp_styling=" << b(s.use_npp_styling) << "\n";
    out << "tree_view_font_size=" << s.tree_view_font_size << "\n";
    out << "minArrayLength=" << s.minArrayLength << "\n";
    out << "maxArrayLength=" << s.maxArrayLength << "\n";
    out << "extended_ascii_strings=" << b(s.extended_ascii_strings) << "\n";
    out << "generate_random_patterns=" << b(s.generate_random_patterns) << "\n";
    out << "max_schema_validation_problems=" << s.max_schema_validation_problems << "\n";
    std::ofstream f(path, std::ios::trunc);
    if (f) f << out.str();
}
