// JsonTools.macos — JtSettings.h: port of Utils/Settings.cs. Same snake_case
// names and defaults as upstream; persisted as <config>/JsonTools/JsonTools.ini.

#pragma once

#include <string>

#include "JNode.h"      // PrettyPrintStyle, KeyStyle, Dtype
#include "JsonParser.h" // LoggerLevel
#include "MiscModules.h" // EndOfLine

enum class AskUserWhetherToDoThing { DONT_DO_DONT_ASK, ASK_BEFORE_DOING, DO_WITHOUT_ASKING };

struct JtSettings {
    // JSON Parser
    jsontools::LoggerLevel logger_level = jsontools::LoggerLevel::NAN_INF;
    bool offer_to_show_lint = true;
    // Performance
    double max_file_size_MB_slow_actions = 4.0;
    bool auto_validate = false;
    int inactivity_seconds_before_parse = 2;
    // Tree View
    int max_json_length_full_tree = 10000;
    bool tree_node_images = true;
    // JSON formatting
    int indent_pretty_print = 4;
    bool tab_indent_pretty_print = false;
    bool minimal_whitespace_compression = true;
    bool sort_keys = false;
    jsontools::PrettyPrintStyle pretty_print_style = jsontools::PrettyPrintStyle::Google;
    bool remember_comments = false;
    AskUserWhetherToDoThing ask_before_pretty_printing_json_lines =
        AskUserWhetherToDoThing::ASK_BEFORE_DOING;
    // Miscellaneous
    jsontools::KeyStyle key_style = jsontools::KeyStyle::RemesPath;
    bool keyboard_shortcuts = true;
    std::string path_separator = "\"\\u0001\""; // JSON string; "\u0001" = unset
    std::string try_parse_start_chars = "\"[{";
    bool skip_api_request_and_fuzz_tests = true;
    jsontools::EndOfLine csv_newline = jsontools::EndOfLine::LF;
    std::string toolbar_icons = "tcpo";
    bool auto_try_guess_csv_delim_newline = false;
    // Styling
    bool use_npp_styling = true;
    float tree_view_font_size = 11.0f; // points; mac default differs from WinForms 7.8
    // Random JSON
    int minArrayLength = 0;
    int maxArrayLength = 10;
    bool extended_ascii_strings = false;
    bool generate_random_patterns = false; // patterns are unsupported in this port
    // JSON Schema
    int max_schema_validation_problems = 64;
};

JtSettings& jtSettings();
void jtSettingsLoad();
void jtSettingsSave();
