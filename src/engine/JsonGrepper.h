// JsonGrepper.h — port of JsonGrepper.cs.
//
// Reads JSON files based on search patterns, and also fetches JSON from APIs.
// Combines all JSON into a map, fnameJsons, from filenames/urls to JSON.
//
// PORT NOTES: Grep() is synchronous (the Cocoa shell runs it off the main
// thread and drives progress via the callbacks); GetJsonFromApis uses
// NSURLSession (implemented in JsonGrepper.mm) and blocks until every request
// completes.

#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "JNode.h"
#include "JsonParser.h"

namespace jsontools {

class JsonGrepper {
public:
    // lower than you might expect: the combined memory of the tree view, the
    // editor buffer, and the JNodes could exhaust the process (upstream's
    // 64-bit figure; the 32-bit path doesn't exist on macOS)
    static constexpr int64_t MAX_COMBINED_LENGTH_TEXT_TO_PARSE = 400000000;
    static constexpr int PROGRESS_REPORT_FILE_READING_MIN_COUNT = 64;
    static constexpr int PROGRESS_REPORT_FILE_MIN_COUNT = 16;
    static constexpr int64_t PROGRESS_REPORT_TEXT_MIN_TOT_LENGTH = 8000000;
    static constexpr int64_t PROGRESS_REPORT_TEXT_MIN_TOT_LENGTH_IF_LT_MINCOUNT_FILES = 50000000;

    // maps filenames and urls to parsed JSON
    std::shared_ptr<JObject> fnameJsons;
    // maps filenames/urls to parse/fetch error strings (as JNode strings)
    std::shared_ptr<JObject> exceptions;
    std::map<std::string, std::string> fnameStrings;
    JsonParser jsonParser;
    // set when ReadJsonFiles stopped because MAX_COMBINED_LENGTH_TEXT_TO_PARSE
    // was exceeded; the shell shows the "Too much text to parse" alert
    bool tooMuchText = false;

    // totalNumber = file count (reading) or combined char count (parsing);
    // totalLengthOnHardDrive = -1 when reporting for parsing
    using ProgressReportSetup = std::function<void(int64_t totalNumber, int64_t totalLengthOnHardDrive)>;
    using ProgressReportCallback = std::function<void(int64_t numberSoFar, int64_t totalNumber)>;

    explicit JsonGrepper(JsonParser parser = JsonParser(LoggerLevel::JSON5),
                         bool reportProgress = false, int progressReportCheckpoints = -1,
                         ProgressReportSetup setup = nullptr,
                         ProgressReportCallback callback = nullptr,
                         std::function<void()> teardown = nullptr)
        : fnameJsons(std::make_shared<JObject>()), exceptions(std::make_shared<JObject>()),
          jsonParser(parser), reportProgress(reportProgress),
          progressReportCheckpoints(progressReportCheckpoints), progressReportSetup(setup),
          progressReportCallback(callback), progressReportTeardown(teardown) {
        jsonParser.throwIfFatal = true;
        jsonParser.throwIfLogged = true;
    }

    bool IsBusy() const { return isBusy; }

    // for each file matching any of searchPatterns in rootDir (and
    // subdirectories if recursive), parse it and record the result in
    // fnameJsons (or exceptions)
    void Grep(const std::string& rootDir, bool recursive,
              const std::vector<std::string>& searchPatterns);

    // fetch each url, then parse everything downloaded (blocking)
    void GetJsonFromApis(const std::vector<std::string>& urls);

    // cancel an in-progress Grep/parse (callable from another thread)
    void Cancel() { cancellationRequested = true; }

    // clear all exceptions, JSON strings, and JSON
    void Reset() {
        fnameStrings.clear();
        fnameJsons->children.clear();
        exceptions->children.clear();
        tooMuchText = false;
    }

private:
    bool isBusy = false;
    int64_t totalLengthToParse = 0;
    bool reportProgress;
    int progressReportCheckpoints;
    ProgressReportSetup progressReportSetup;
    ProgressReportCallback progressReportCallback;
    std::function<void()> progressReportTeardown;
    std::atomic<bool> cancellationRequested{false};

    struct FileEntry {
        std::string path;
        int64_t size;
    };
    bool TryGetFilesToRead(const std::string& rootDir, bool recursive,
                           std::vector<FileEntry>& allFiles, int64_t& totalLengthToRead,
                           bool& shouldReportProgress);
    void ReadJsonFiles(const std::vector<FileEntry>& allFiles, bool shouldReportProgress,
                       const std::vector<std::string>& searchPatterns);
    void ParseJsonStrings();
    // implemented in JsonGrepper.mm (NSURLSession); fills fnameStrings/exceptions
    void FetchUrls(const std::vector<std::string>& urls);
};

} // namespace jsontools
