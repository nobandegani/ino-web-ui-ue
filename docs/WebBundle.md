# Web Bundle assets

Shipping a `Content/web/` folder of loose files works, but has two
problems:

1. It clutters packaging settings
   (*Additional Non-Asset Directories to Copy*).
2. Those files sit on the end-user's disk in plain sight, outside the
   `.pak`.

`UInoWebBundle` is an alternative: a first-class Unreal asset that
holds your built web content inside itself and extracts it on demand at
runtime.

- It ships inside the `.pak` (encrypted if pak encryption is on).
- It appears in the Content Browser under a custom **Ino** category.
- It re-bundles via right-click -> Reimport Source Folder.

## Creating a bundle

1. Right-click in the Content Browser -> **Ino** -> **Web Bundle**.
2. Name it — for example, `MainUI`.
3. Open it and set the detail-panel fields:

   | Field | Example |
   |---|---|
   | `SourceFolder`      | `WebUI/dist` (relative to `Content/`) |
   | `InitialURL`        | `https://ui.local/index.html` |
   | `VirtualHostName`   | `ui.local` |

4. Right-click the asset -> **Reimport Source Folder**.
   The `Files[]` array fills in, `ContentHash` is computed, the asset
   is marked dirty.
5. Save with Ctrl+S.

## Using a bundle

### C++

```cpp
UInoWebUISubsystem* WebUI = GI->GetSubsystem<UInoWebUISubsystem>();
UInoWebView* View = WebUI->CreateWebViewFromAsset(TEXT("MainUI"), MyBundle);
```

### Blueprint

Call the **Create Web View From Bundle** node off the subsystem.

## Runtime behaviour

| Build type | Behaviour |
|---|---|
| Editor / uncooked | Serves loose files directly from `SourceFolder`. Hot-iteration friendly — edit the file on disk and reload the WebView, no extraction step. |
| Packaged / cooked | Extracts `Files[]` to `<ProjectSavedDir>/InoWebBundles/<AssetName>/` on first use. A hash-sidecar (`.inowebbundle.hash`) keeps re-extraction from happening until the asset's `ContentHash` changes between patches. |

## Storage model

- `Files[]` is `TArray<FInoWebBundleFile>`.
- Each entry is `{ RelativePath (POSIX style), Bytes (uncompressed) }`.
- `ContentHash` is MD5 hex over the sorted `(path, bytes)` tuples.
- No compression is applied inside the asset itself — the `.pak` layer
  already compresses, so compressing twice would only waste CPU.

## Source layout

```
Source/
|-- InoWebUI/                                  runtime (Win64 + Android)
|   |-- Public/InoWebBundle.h                  UObject + FInoWebBundleFile
|   `-- Private/InoWebBundle.cpp               BundleFromFolder,
|                                               ExtractToDirectory,
|                                               ComputeHash
`-- InoWebUIEditor/                            editor-only module
    `-- Private/
        |-- InoWebBundleFactory.cpp            "New Asset -> Ino -> Web Bundle"
        `-- InoWebBundleActions.cpp            Content Browser menu + Reimport
```

## Path resolution warning

On Android packaged builds, `FPaths::ConvertRelativePathToFull` does
not fully resolve the virtual `../../../Project/...` path that UE uses
internally — but the Android `FileInputStream` opened from Java does
not understand that form. The bundle extractor handles this for you
via
`IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(...)`,
and the resulting folder is what gets registered with the virtual-host
mapping. If you write your own extractor and hand paths to native code
outside UE, use the same conversion — see
`UInoWebUISubsystem::ResolveBundleContentFolder` for the reference
implementation.
