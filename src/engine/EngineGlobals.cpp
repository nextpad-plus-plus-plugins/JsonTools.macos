// EngineGlobals.cpp — globals shared between JNode/RemesPath translation units.
// ArgFunctionInitializeGlobals grows real content when RemesPathFunctions.cpp
// lands; JQueryContext already calls it (C# parity: ArgFunction.InitializeGlobals).

#include "JNode.h"

namespace jsontools {

bool regexSearchResultsShouldBeCached = true;

// forward-declared in JNode.cpp; the real reset work is registered by
// RemesPathFunctions.cpp through this hook so JNode.cpp never needs to know
// about the function registry.
static void (*gInitializeGlobalsHook)(bool) = nullptr;

void SetArgFunctionInitializeGlobalsHook(void (*hook)(bool)) {
    gInitializeGlobalsHook = hook;
}

void ArgFunctionInitializeGlobals(bool mutatesInput) {
    if (gInitializeGlobalsHook) gInitializeGlobalsHook(mutatesInput);
}

} // namespace jsontools
