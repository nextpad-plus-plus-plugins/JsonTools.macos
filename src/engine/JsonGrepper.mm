// JsonGrepper.mm — see JsonGrepper.h.

#import <Foundation/Foundation.h>

#include "JsonGrepper.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "Glob.h"

namespace fs = std::filesystem;

namespace jsontools {

bool JsonGrepper::TryGetFilesToRead(const std::string& rootDir, bool recursive,
                                    std::vector<FileEntry>& allFiles, int64_t& totalLengthToRead,
                                    bool& shouldReportProgress) {
    allFiles.clear();
    shouldReportProgress = false;
    totalLengthToRead = -1;
    std::error_code ec;
    if (!fs::is_directory(fs::path(rootDir), ec) || ec) return false;
    totalLengthToRead = 0;
    auto addEntry = [&](const fs::directory_entry& entry) {
        std::error_code ec2;
        if (!entry.is_regular_file(ec2) || ec2) return;
        int64_t size = static_cast<int64_t>(entry.file_size(ec2));
        if (ec2) size = 0;
        allFiles.push_back({entry.path().string(), size});
        totalLengthToRead += size;
    };
    if (recursive) {
        fs::recursive_directory_iterator it(rootDir, fs::directory_options::skip_permission_denied, ec);
        if (ec) return false;
        for (auto end = fs::end(it); it != end; it.increment(ec)) {
            if (ec) break;
            addEntry(*it);
        }
    } else {
        fs::directory_iterator it(rootDir, fs::directory_options::skip_permission_denied, ec);
        if (ec) return false;
        for (const auto& entry : it) addEntry(entry);
    }
    int64_t nFiles = static_cast<int64_t>(allFiles.size());
    shouldReportProgress = reportProgress &&
        (totalLengthToRead >= PROGRESS_REPORT_TEXT_MIN_TOT_LENGTH_IF_LT_MINCOUNT_FILES ||
         (totalLengthToRead >= PROGRESS_REPORT_TEXT_MIN_TOT_LENGTH &&
          nFiles >= PROGRESS_REPORT_FILE_READING_MIN_COUNT));
    return true;
}

void JsonGrepper::ReadJsonFiles(const std::vector<FileEntry>& allFiles, bool shouldReportProgress,
                                const std::vector<std::string>& searchPatterns) {
    totalLengthToParse = 0;
    int64_t nFiles = static_cast<int64_t>(allFiles.size());
    int64_t checkPointLength = progressReportCheckpoints > 0 ? nFiles / progressReportCheckpoints : nFiles;
    if (checkPointLength <= 0) checkPointLength = 1;
    int64_t nextCheckPoint = checkPointLength;
    std::string joined;
    for (size_t i = 0; i < searchPatterns.size(); i++) {
        if (i > 0) joined += '\n';
        joined += searchPatterns[i];
    }
    Glob glob;
    auto globFunc = glob.ParseLinesSimple(joined);
    for (int64_t filesReadSoFar = 0; filesReadSoFar < nFiles; filesReadSoFar++) {
        if (cancellationRequested) return;
        const FileEntry& fileInfo = allFiles[static_cast<size_t>(filesReadSoFar)];
        if (globFunc(fileInfo.path)) {
            std::ifstream fp(fileInfo.path, std::ios::binary);
            if (fp) {
                std::ostringstream ss;
                ss << fp.rdbuf();
                std::string text = ss.str();
                totalLengthToParse += static_cast<int64_t>(text.size());
                if (totalLengthToParse > MAX_COMBINED_LENGTH_TEXT_TO_PARSE) {
                    // quit if there's too much text, on the expectation that
                    // parsing everything would run the process out of memory
                    tooMuchText = true;
                    cancellationRequested = true;
                    return;
                }
                fnameStrings[fileInfo.path] = std::move(text);
            }
        }
        if (shouldReportProgress && progressReportCallback && filesReadSoFar == nextCheckPoint) {
            nextCheckPoint = nextCheckPoint + checkPointLength > nFiles ? nFiles
                                                                        : nextCheckPoint + checkPointLength;
            progressReportCallback(filesReadSoFar, nFiles);
        }
    }
}

void JsonGrepper::ParseJsonStrings() {
    bool shouldReportProgress = reportProgress &&
        (totalLengthToParse >= PROGRESS_REPORT_TEXT_MIN_TOT_LENGTH_IF_LT_MINCOUNT_FILES ||
         (totalLengthToParse >= PROGRESS_REPORT_TEXT_MIN_TOT_LENGTH &&
          static_cast<int64_t>(fnameStrings.size()) >= PROGRESS_REPORT_FILE_MIN_COUNT));
    int64_t totLengthAlreadyParsed = 0;
    int64_t checkPointLength =
        progressReportCheckpoints > 0 ? totalLengthToParse / progressReportCheckpoints : totalLengthToParse;
    if (checkPointLength <= 0) checkPointLength = 1;
    int64_t nextCheckPoint = checkPointLength;
    if (shouldReportProgress) {
        if (progressReportSetup) progressReportSetup(totalLengthToParse, -1);
        if (progressReportCallback) progressReportCallback(0, totalLengthToParse);
    }
    // std::map iterates in sorted key order, matching upstream's OrderBy(fname)
    for (const auto& [fname, jsonStr] : fnameStrings) {
        if (cancellationRequested) break;
        JNodePtr parsedOrError;
        bool error = false;
        try {
            JsonParser parser = jsonParser.Copy();
            parsedOrError = parser.Parse(jsonStr);
        } catch (const std::exception& ex) {
            error = true;
            parsedOrError = JNode::Str(ex.what(), 0);
        }
        if (error)
            exceptions->children.set(fname, parsedOrError);
        else
            fnameJsons->children.set(fname, parsedOrError);
        if (shouldReportProgress && progressReportCallback && nextCheckPoint < totalLengthToParse) {
            totLengthAlreadyParsed += static_cast<int64_t>(jsonStr.size());
            if (totLengthAlreadyParsed >= nextCheckPoint) {
                progressReportCallback(totLengthAlreadyParsed, totalLengthToParse);
                nextCheckPoint = totLengthAlreadyParsed + checkPointLength;
                if (nextCheckPoint > totalLengthToParse) nextCheckPoint = totalLengthToParse;
            }
        }
    }
    if (shouldReportProgress) {
        if (progressReportCallback) progressReportCallback(totalLengthToParse, totalLengthToParse);
        if (progressReportTeardown) progressReportTeardown();
    }
    fnameStrings.clear(); // don't need the strings anymore, only the JSON
}

void JsonGrepper::Grep(const std::string& rootDir, bool recursive,
                       const std::vector<std::string>& searchPatterns) {
    std::vector<FileEntry> allFiles;
    int64_t totalLengthToRead;
    bool shouldReportProgress;
    if (!TryGetFilesToRead(rootDir, recursive, allFiles, totalLengthToRead, shouldReportProgress))
        return;
    isBusy = true;
    tooMuchText = false;
    cancellationRequested = false;
    int64_t nFiles = static_cast<int64_t>(allFiles.size());
    if (shouldReportProgress) {
        if (progressReportSetup) progressReportSetup(nFiles, totalLengthToRead);
        if (progressReportCallback) progressReportCallback(0, nFiles);
    }
    ReadJsonFiles(allFiles, shouldReportProgress, searchPatterns);
    if (shouldReportProgress) {
        if (progressReportCallback) progressReportCallback(nFiles, nFiles);
        if (progressReportTeardown) progressReportTeardown();
    }
    if (cancellationRequested) {
        isBusy = false;
        return; // the shell checks tooMuchText and shows the alert
    }
    ParseJsonStrings();
    isBusy = false;
}

void JsonGrepper::FetchUrls(const std::vector<std::string>& urls) {
    @autoreleasepool {
        NSURLSessionConfiguration* config = [NSURLSessionConfiguration ephemeralSessionConfiguration];
        NSURLSession* session = [NSURLSession sessionWithConfiguration:config];
        dispatch_group_t group = dispatch_group_create();
        // collect results on a serial queue, then merge on the caller's thread
        NSMutableDictionary<NSNumber*, NSString*>* bodies = [NSMutableDictionary dictionary];
        NSMutableDictionary<NSNumber*, NSString*>* errors = [NSMutableDictionary dictionary];
        dispatch_queue_t resultQueue =
            dispatch_queue_create("org.nextpadplusplus.jsontools.grepper", DISPATCH_QUEUE_SERIAL);
        for (size_t i = 0; i < urls.size(); i++) {
            NSNumber* key = @(i);
            NSString* urlStr = [NSString stringWithUTF8String:urls[i].c_str()];
            NSURL* url = [NSURL URLWithString:urlStr];
            if (!url) {
                errors[key] = [NSString stringWithFormat:@"invalid URL: %@", urlStr];
                continue;
            }
            NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:url];
            [request setValue:@"application/json" forHTTPHeaderField:@"Accept"];
            [request setValue:@"JsonTools Nextpad++ plugin" forHTTPHeaderField:@"User-Agent"];
            dispatch_group_enter(group);
            NSURLSessionDataTask* task = [session
                dataTaskWithRequest:request
                  completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
                    NSString* body = nil;
                    NSString* errStr = nil;
                    if (error) {
                        errStr = error.localizedDescription;
                    } else {
                        NSInteger status = 0;
                        if ([response isKindOfClass:[NSHTTPURLResponse class]])
                            status = ((NSHTTPURLResponse*)response).statusCode;
                        if (status >= 400) {
                            errStr = [NSString stringWithFormat:@"HTTP error %ld", (long)status];
                        } else {
                            body = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
                            if (!body) errStr = @"response was not valid UTF-8";
                        }
                    }
                    dispatch_async(resultQueue, ^{
                      if (body)
                          bodies[key] = body;
                      else
                          errors[key] = errStr ?: @"unknown error";
                      dispatch_group_leave(group);
                    });
                  }];
            [task resume];
        }
        dispatch_group_wait(group, dispatch_time(DISPATCH_TIME_NOW, 60 * NSEC_PER_SEC));
        // barrier: everything queued on resultQueue is done after this sync
        dispatch_sync(resultQueue, ^{});
        for (size_t i = 0; i < urls.size(); i++) {
            NSNumber* key = @(i);
            NSString* body = bodies[key];
            NSString* errStr = errors[key];
            if (body) {
                fnameStrings[urls[i]] = std::string([body UTF8String]);
            } else {
                std::string msg = errStr ? [errStr UTF8String] : "request timed out";
                exceptions->children.set(urls[i], JNode::Str(msg, 0));
            }
        }
        [session invalidateAndCancel];
    }
}

void JsonGrepper::GetJsonFromApis(const std::vector<std::string>& urls) {
    isBusy = true;
    cancellationRequested = false;
    totalLengthToParse = 0;
    FetchUrls(urls);
    for (const auto& kv : fnameStrings) totalLengthToParse += static_cast<int64_t>(kv.second.size());
    ParseJsonStrings();
    isBusy = false;
}

} // namespace jsontools
