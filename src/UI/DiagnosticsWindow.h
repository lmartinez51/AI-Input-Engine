#pragma once

#include <windows.h>

#include <memory>

#include "Translation/TranslationTypes.h"

namespace aiie {

// Non-modal diagnostics window: log ring buffer + provider status.
// Reuses a single instance if already open (IsDiagnosticsOpen).
void ShowDiagnosticsWindow(std::shared_ptr<ITranslationProvider> provider, HWND parent = nullptr);
bool IsDiagnosticsOpen();

} // namespace aiie