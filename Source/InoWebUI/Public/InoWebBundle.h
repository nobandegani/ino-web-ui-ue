// Copyright Inoland. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Engine/EngineTypes.h"   // FDirectoryPath
#include "InoWebUITypes.h"        // FInoWebViewConfig (embedded below)
#include "InoWebBundle.generated.h"

/**
 * One file inside a UInoWebBundle — relative path + raw bytes.
 * Populated at editor-time by UInoWebBundle::BundleFromFolder; serialized
 * into the asset on save and restored on load.
 */
USTRUCT()
struct FInoWebBundleFile
{
    GENERATED_BODY()

    /** POSIX-style relative path from the bundle root ("index.html", "assets/app.js"). */
    UPROPERTY()
    FString RelativePath;

    /** Raw file bytes. Stored uncompressed — UE's .pak handles compression at cook. */
    UPROPERTY()
    TArray<uint8> Bytes;
};

/**
 * UInoWebBundle — a self-contained blob of web content (HTML/JS/CSS/etc.)
 * bundled as a UE asset.
 *
 * Dev workflow:
 *   1. Drop a folder (Vite/Webpack dist, or hand-written HTML) into your project.
 *   2. Create a WebBundle asset, set SourceFolder to that folder.
 *   3. Set InitialURL + VirtualHostName.
 *   4. Click Reimport in the asset editor — all the files are read into the
 *      asset, hashed, and saved.
 *
 * Runtime usage:
 *   UInoWebView* View = Subsystem->CreateWebViewFromAsset(TEXT("MainUI"), Bundle);
 *
 * Behavior depends on build:
 *   • Editor / non-cooked: serves loose files directly from SourceFolder.
 *   • Packaged / cooked:   on first use, extracts Files[] into
 *                          Saved/InoWebBundles/<AssetName>/ (if stale or
 *                          missing), then virtual-host-maps that directory.
 */
UCLASS(BlueprintType)
class INOWEBUI_API UInoWebBundle : public UObject
{
    GENERATED_BODY()

public:

    // ── Editor-configured ───────────────────────────────────────────────────

    /**
     * Full FInoWebViewConfig used when this bundle creates a WebView.
     * Set InitialURL, VirtualHostName, transparency, lockdown, dialog
     * blocking, devtools, etc. here — all the same fields you'd pass to
     * UInoWebUISubsystem::CreateWebView.
     *
     * VirtualHostFolder inside Config is IGNORED when the bundle is used —
     * the bundle always provides the folder itself (from SourceFolder in
     * editor, or from the extracted Files[] in packaged builds).
     */
    UPROPERTY(EditAnywhere, Category = "InoWebBundle", meta = (ShowOnlyInnerProperties))
    FInoWebViewConfig Config;

    /**
     * Folder on disk to read from at Reimport time. Relative paths are
     * resolved against the project's Content/ directory; absolute paths are
     * used verbatim. Only read in the editor — in packaged builds this
     * field is informational and Files[] is the source of truth.
     */
    UPROPERTY(EditAnywhere, Category = "InoWebBundle")
    FDirectoryPath SourceFolder;

    /**
     * Optional override used ONLY in editor / uncooked builds (PIE,
     * Standalone Game launched from the editor). When non-empty, overrides
     * Config.InitialURL so you can point the WebView at a live dev server
     * (Vite, webpack-dev-server, etc.) with hot-module reload. In packaged
     * / cooked builds this field is ignored — Config.InitialURL is used
     * and the virtual-host mapping serves the bundled files.
     *
     * When this URL is used, the plugin additionally:
     *   • clears VirtualHostName / VirtualHostFolder (the dev server
     *     serves its own URLs, not the virtual host),
     *   • disables bLockToVirtualHost (so lockdown doesn't block the
     *     dev URL you just asked for).
     *
     * Leave empty to use Config.InitialURL + the virtual host in every
     * build type.
     *
     * Typical value:  http://localhost:5173
     */
    UPROPERTY(EditAnywhere, Category = "InoWebBundle")
    FString DevInitialURL;

    /**
     * User-supplied version string, e.g. "1.4.2" or a git short SHA.
     * Optional. Useful for cache-busting (BP can append `?v=<version>` to
     * URLs) and for showing the bundled UI's version on an About screen.
     * Not consumed by the plugin itself — purely informational.
     */
    UPROPERTY(EditAnywhere, Category = "InoWebBundle")
    FString BundleVersion;

    /**
     * UE wildcard patterns matched against POSIX-style paths relative to
     * SourceFolder. Any file whose path matches any pattern is skipped
     * during BundleFromFolder. Defaults catch the common junk that finds
     * its way into web build outputs — `.git/`, `.DS_Store`, source maps,
     * `.env*`, etc. — so an accidental "I bundled my whole project root"
     * doesn't ship secrets or megabytes of node_modules.
     *
     * Customize per-asset to add or override.
     */
    UPROPERTY(EditAnywhere, Category = "InoWebBundle")
    TArray<FString> ExcludePatterns = {
        TEXT(".git/*"), TEXT(".gitignore"), TEXT(".gitattributes"),
        TEXT(".DS_Store"), TEXT("Thumbs.db"), TEXT("desktop.ini"),
        TEXT("node_modules/*"),
        TEXT("*.map"),
        TEXT(".env"), TEXT(".env.*"),
        TEXT("*.bak"), TEXT("*.tmp"), TEXT("*.swp")
    };

    // ── Baked at Reimport (read-only at runtime) ────────────────────────────

    /** All files captured from SourceFolder at Reimport. Sorted by relative path. */
    UPROPERTY(VisibleAnywhere, Category = "InoWebBundle|Baked")
    TArray<FInoWebBundleFile> Files;

    /** MD5 hex of the bundled content. Doubles as a version tag for the on-disk
     *  extraction sidecar so we can detect stale extractions. */
    UPROPERTY(VisibleAnywhere, Category = "InoWebBundle|Baked")
    FString ContentHash;

    /** Total uncompressed byte count across all Files. Diagnostic only. */
    UPROPERTY(VisibleAnywhere, Category = "InoWebBundle|Baked")
    int64 TotalBytes = 0;

    // ── Editor + runtime operations ─────────────────────────────────────────

    /**
     * Walk AbsoluteFolder recursively, read every file into Files[], compute
     * ContentHash, update TotalBytes. Returns true on success, false if the
     * folder can't be read or is empty. Clears prior contents on success.
     */
    bool BundleFromFolder(const FString& AbsoluteFolder);

    /**
     * Write Files[] to DestFolder as loose files. Clears DestFolder first.
     * Used by the runtime extractor when loading a packaged bundle.
     */
    bool ExtractToDirectory(const FString& DestFolder) const;

    /**
     * Compute an MD5 hash over every file's relative path + bytes. Stable
     * across runs because Files[] is sorted in BundleFromFolder.
     */
    FString ComputeHash() const;

    /**
     * Resolve SourceFolder to an absolute disk path. Relative paths anchor
     * at the project's Content/ directory (same rule as LoadLocalFile and
     * FInoWebViewConfig::VirtualHostFolder). Returns empty if unset.
     */
    FString GetAbsoluteSourceFolder() const;
};
