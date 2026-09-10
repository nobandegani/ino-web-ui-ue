// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "Logging/LogMacros.h"

/**
 * Log category for the InoWebUI plugin.
 *
 * Control verbosity from DefaultEngine.ini:
 *   [Core.Log]
 *   LogInoWebUI=Verbose
 */
INOWEBUI_API DECLARE_LOG_CATEGORY_EXTERN(LogInoWebUI, Log, All);
