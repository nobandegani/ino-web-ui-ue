// Copyright Inoland. All Rights Reserved.

#include "InoWebBundle.h"
#include "InoWebUILog.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"        // FMD5
#include "Misc/Char.h"

FString UInoWebBundle::GetAbsoluteSourceFolder() const
{
    const FString& Input = SourceFolder.Path;
    if (Input.IsEmpty()) return FString();

    if (FPaths::IsRelative(Input))
    {
        return FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / Input);
    }
    return FPaths::ConvertRelativePathToFull(Input);
}

// ─────────────────────────────────────────────────────────────────────────────
//  BundleFromFolder — the "Reimport" workhorse.
// ─────────────────────────────────────────────────────────────────────────────
bool UInoWebBundle::BundleFromFolder(const FString& AbsoluteFolder)
{
    IFileManager& FM = IFileManager::Get();

    if (AbsoluteFolder.IsEmpty())
    {
        UE_LOG(LogInoWebUI, Warning, TEXT("UInoWebBundle::BundleFromFolder: empty path."));
        return false;
    }
    if (!FM.DirectoryExists(*AbsoluteFolder))
    {
        UE_LOG(LogInoWebUI, Warning,
            TEXT("UInoWebBundle::BundleFromFolder: directory not found: %s"), *AbsoluteFolder);
        return false;
    }

    // Recursively list every file under the folder.
    TArray<FString> FoundFiles;
    FM.FindFilesRecursive(FoundFiles, *AbsoluteFolder, TEXT("*"), /*Files=*/true, /*Dirs=*/false);

    if (FoundFiles.Num() == 0)
    {
        UE_LOG(LogInoWebUI, Warning,
            TEXT("UInoWebBundle::BundleFromFolder: no files under %s"), *AbsoluteFolder);
        Files.Reset();
        ContentHash.Reset();
        TotalBytes = 0;
        return false;
    }

    // Trailing slash so MakePathRelativeTo produces clean relative paths.
    const FString Root = (AbsoluteFolder.EndsWith(TEXT("/")) || AbsoluteFolder.EndsWith(TEXT("\\")))
        ? AbsoluteFolder
        : (AbsoluteFolder + TEXT("/"));

    // Filter against ExcludePatterns. Patterns match the POSIX-style path
    // relative to Root, so users can write rules like ".git/*" or
    // "node_modules/*" the way they would in a .gitignore.
    int32 SkippedCount = 0;
    if (ExcludePatterns.Num() > 0)
    {
        FoundFiles.RemoveAll([&](const FString& AbsPath)
        {
            FString Rel = AbsPath;
            FPaths::MakePathRelativeTo(Rel, *Root);
            Rel.ReplaceInline(TEXT("\\"), TEXT("/"));
            for (const FString& Pattern : ExcludePatterns)
            {
                if (Rel.MatchesWildcard(Pattern))
                {
                    UE_LOG(LogInoWebUI, Verbose,
                        TEXT("  excluded (matches '%s'): %s"), *Pattern, *Rel);
                    ++SkippedCount;
                    return true;
                }
            }
            return false;
        });
    }
    if (SkippedCount > 0)
    {
        UE_LOG(LogInoWebUI, Log,
            TEXT("Bundle '%s': %d file(s) skipped by ExcludePatterns."),
            *GetName(), SkippedCount);
    }
    if (FoundFiles.Num() == 0)
    {
        UE_LOG(LogInoWebUI, Warning,
            TEXT("UInoWebBundle::BundleFromFolder: every candidate file was "
                 "excluded by ExcludePatterns under %s"), *AbsoluteFolder);
        return false;
    }

    // Sort for stable hash output across machines / runs.
    FoundFiles.Sort();

    TArray<FInoWebBundleFile> NewFiles;
    NewFiles.Reserve(FoundFiles.Num());
    int64 NewTotalBytes = 0;

    for (const FString& FullPath : FoundFiles)
    {
        FInoWebBundleFile Entry;

        // Compute path relative to the bundle root.
        Entry.RelativePath = FullPath;
        FPaths::MakePathRelativeTo(Entry.RelativePath, *Root);
        Entry.RelativePath.ReplaceInline(TEXT("\\"), TEXT("/"));

        // Read the file bytes.
        if (!FFileHelper::LoadFileToArray(Entry.Bytes, *FullPath))
        {
            UE_LOG(LogInoWebUI, Warning,
                TEXT("  skipping (read failed): %s"), *FullPath);
            continue;
        }

        NewTotalBytes += Entry.Bytes.Num();
        NewFiles.Add(MoveTemp(Entry));
    }

    if (NewFiles.Num() == 0)
    {
        UE_LOG(LogInoWebUI, Warning,
            TEXT("UInoWebBundle::BundleFromFolder: all candidate files failed to read."));
        return false;
    }

    // Commit atomically — replace Files[] only on success.
    Files      = MoveTemp(NewFiles);
    TotalBytes = NewTotalBytes;
    ContentHash = ComputeHash();

    UE_LOG(LogInoWebUI, Log,
        TEXT("Bundle '%s' imported: %d file(s), %lld bytes, hash=%s"),
        *GetName(), Files.Num(), TotalBytes, *ContentHash);

    // Mark the asset dirty so the editor offers to save it. No-op outside editor.
    MarkPackageDirty();
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  ExtractToDirectory — runtime use, called on first access in packaged builds.
// ─────────────────────────────────────────────────────────────────────────────
bool UInoWebBundle::ExtractToDirectory(const FString& DestFolder) const
{
    if (Files.Num() == 0)
    {
        UE_LOG(LogInoWebUI, Warning,
            TEXT("ExtractToDirectory: bundle '%s' has no files."), *GetName());
        return false;
    }
    if (DestFolder.IsEmpty())
    {
        UE_LOG(LogInoWebUI, Warning, TEXT("ExtractToDirectory: empty destination."));
        return false;
    }

    IFileManager& FM = IFileManager::Get();

    // Stage to a sibling temp dir so a mid-extraction failure leaves the
    // existing dest intact (better than the pre-fix behavior, which would
    // delete dest first and then leave it half-written if any file failed).
    const FString StagingFolder = DestFolder + TEXT(".staging");

    // Clean any leftover staging from a previously aborted run.
    if (FM.DirectoryExists(*StagingFolder))
    {
        FM.DeleteDirectory(*StagingFolder, /*RequireExists=*/false, /*Tree=*/true);
    }
    if (!FM.MakeDirectory(*StagingFolder, /*Tree=*/true))
    {
        UE_LOG(LogInoWebUI, Error,
            TEXT("ExtractToDirectory: could not create staging dir %s"), *StagingFolder);
        return false;
    }

    // Write every file into staging. Any failure aborts and leaves the
    // current dest untouched.
    for (const FInoWebBundleFile& File : Files)
    {
        const FString FullDest = StagingFolder / File.RelativePath;

        // SaveArrayToFile creates missing parent directories.
        if (!FFileHelper::SaveArrayToFile(File.Bytes, *FullDest))
        {
            UE_LOG(LogInoWebUI, Error,
                TEXT("ExtractToDirectory: write failed: %s"), *FullDest);
            FM.DeleteDirectory(*StagingFolder, /*RequireExists=*/false, /*Tree=*/true);
            return false;
        }
    }

    // Swap into place. Wipe the old dest, then move staging onto it. Both
    // dirs live under ProjectSavedDir/InoWebBundles/, so the rename is
    // same-volume on every supported platform.
    if (FM.DirectoryExists(*DestFolder))
    {
        FM.DeleteDirectory(*DestFolder, /*RequireExists=*/false, /*Tree=*/true);
    }
    if (!FM.Move(*DestFolder, *StagingFolder))
    {
        UE_LOG(LogInoWebUI, Error,
            TEXT("ExtractToDirectory: rename %s -> %s failed."),
            *StagingFolder, *DestFolder);
        return false;
    }

    UE_LOG(LogInoWebUI, Log,
        TEXT("Bundle '%s' extracted to %s (%d files, %lld bytes)"),
        *GetName(), *DestFolder, Files.Num(), TotalBytes);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  ComputeHash — MD5 over every file's path + bytes, in sorted order.
//  Stable across runs because BundleFromFolder sorts Files[] by path.
// ─────────────────────────────────────────────────────────────────────────────
FString UInoWebBundle::ComputeHash() const
{
    FMD5 Md5;

    for (const FInoWebBundleFile& File : Files)
    {
        // Hash the relative path as UTF-8 so hashes match across platforms
        // (TCHAR widths differ).
        FTCHARToUTF8 PathUtf8(*File.RelativePath);
        Md5.Update(
            reinterpret_cast<const uint8*>(PathUtf8.Get()),
            PathUtf8.Length());

        // Separator between path and bytes so "abc"+"def" ≠ "abcd"+"ef".
        const uint8 Sep = 0x1F;
        Md5.Update(&Sep, 1);

        if (File.Bytes.Num() > 0)
        {
            Md5.Update(File.Bytes.GetData(), File.Bytes.Num());
        }
    }

    uint8 Digest[16];
    Md5.Final(Digest);

    FString Hex;
    Hex.Reserve(32);
    for (uint8 B : Digest)
    {
        Hex += FString::Printf(TEXT("%02x"), B);
    }
    return Hex;
}
