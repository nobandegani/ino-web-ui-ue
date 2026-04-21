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
   | `DevInitialURL`     | `http://localhost:5173` *(optional, dev only)* |

4. Right-click the asset -> **Reimport Source Folder**.
   The `Files[]` array fills in, `ContentHash` is computed, the asset
   is marked dirty.
5. Save with Ctrl+S.

### Dev server vs packaged — `DevInitialURL`

Typical web toolchains (Vite, webpack-dev-server) have one URL during
development (with HMR) and a different URL in shipped builds (static
files). The bundle supports this without custom code:

| Field | When used | Typical value |
|---|---|---|
| `Config.InitialURL` | Packaged / cooked builds | `https://ui.local/index.html` |
| `DevInitialURL`     | Editor / uncooked builds, if non-empty | `http://localhost:5173` |

When `DevInitialURL` is non-empty AND you're running in the editor
(PIE, or Standalone Game launched from the editor), the plugin:

- Uses `DevInitialURL` as the initial URL.
- Skips the virtual-host mapping (the dev server serves its own origin).
- Disables `bLockToVirtualHost` (otherwise the dev URL would be
  blocked by the navigation allowlist).

In packaged / cooked builds, `DevInitialURL` is ignored completely —
`Config.InitialURL` is used with the virtual host mapped onto the
extracted bundle contents. No separate build configuration, no
conditional Blueprint branches: set both URLs once on the asset and
the plugin picks the right one per build type.

Leave `DevInitialURL` empty to use `Config.InitialURL` everywhere —
that's the right choice if your dev flow already uses the virtual-host
URL against loose files in `SourceFolder`.

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
