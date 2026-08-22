#pragma once

#include <windows.h>

#include "Configuration/AppConfig.h"

namespace aiie {

// Modal settings dialog bound to an AppConfig.
bool ShowSettingsDialog(HWND parent, AppConfig& config);

} // namespace aiie