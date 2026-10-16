// JsonTools.macos — GrepperForm.h: port of Forms/GrepperForm.cs. A floating
// window for pulling JSON out of local files and APIs, with its own tree.

#pragma once

#import <Cocoa/Cocoa.h>

#include "JsonGrepper.h"

@class JTTreeViewer;

@interface JTGrepperForm : NSObject

// nil when the form has never been opened or was closed
+ (JTGrepperForm*)shared;
+ (void)showOrFocus;

- (jsontools::JsonGrepper*)grepper; // owned by the form
- (JTTreeViewer*)tv;                // nil until "View results" clicked

- (void)applyStyling;
- (void)closeForm;

@end
