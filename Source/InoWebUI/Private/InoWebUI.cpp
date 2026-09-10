// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoWebUI.h"
#include "InoWebUILog.h"

#define LOCTEXT_NAMESPACE "FInoWebUIModule"

// Defines the LogInoWebUI category declared in InoWebUILog.h.
DEFINE_LOG_CATEGORY(LogInoWebUI);

void FInoWebUIModule::StartupModule()
{
    UE_LOG(LogInoWebUI, Log, TEXT("InoWebUI module started."));
}

void FInoWebUIModule::ShutdownModule()
{
    UE_LOG(LogInoWebUI, Log, TEXT("InoWebUI module shut down."));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FInoWebUIModule, InoWebUI)
