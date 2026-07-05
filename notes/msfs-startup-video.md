# MSFS startup/cutscene video under DXVK + vkd3d-proton

## Symptom (refined)
- MSFS 2020/2024 through the msfs-vulkan setup plays video **audio** but shows **no
  picture** (startup/loading/cutscene). Game still reaches the menu.
- The **same DXVK + vkd3d-proton stack works on Linux/Proton**. Only Windows is broken.

## What the stack actually is (from the launcher)
`msfs2024-vulkan/runtime.lock.json` injects, next to the game exe:
- vkd3d-proton: `d3d12.dll`, `d3d12core.dll`  (D3D12 -> Vulkan)
- DXVK:         `d3d11.dll`, `dxgi.dll`        (D3D11 / DXGI -> Vulkan)

So the whole D3D stack is Vulkan-backed on Windows, exactly like Linux. "Missing d3d11"
is **not** the problem.

## Root cause reasoning
vkd3d-proton is identical code on Linux and Windows, so the Linux-works / Windows-fails
delta is **not** in this repo. The only component that differs is **Media Foundation**:

- **Linux/Proton:** Wine's Media Foundation (winegstreamer) software-decodes the video and
  hands frames to DXVK as normal textures. Audio + picture both work.
- **Windows:** *native* Media Foundation decodes (audio proves the pipeline runs) and
  drives the **D3D11 video path** (`ID3D11VideoDevice` / `ID3D11VideoContext::VideoProcessorBlt`
  / DXVA) on what is now **DXVK's** d3d11.dll. If DXVK's D3D11-video / MF interop path
  doesn't deliver the frame the way native MF expects, you get **audio but no picture** —
  audio needs no GPU, video does.

Conclusion: the failing step is the **D3D11 / Media Foundation video path (DXVK side)**,
not the D3D12 renderer (vkd3d). The included diagnostic exists to *prove* that in one
launch (see below) and to catch the alternative case where MSFS shares the decoded frame
into D3D12 (then it would be a vkd3d `OpenSharedHandle` issue we can fix here).

## Important: env vars may not reach MSFS 2024
MSFS 2024 is a **Store/Xbox app** (`C:\XboxGames\...`). Store apps frequently do **not**
inherit environment variables set by a parent process, so the launcher's
`VKD3D_CONFIG` / `VKD3D_DEBUG` / `DXVK_LOG_LEVEL` may silently not apply. That is why the
diagnostic below is **env-var-free** and auto-enables by detecting the MSFS exe.

## The diagnostic added (fork-local, env-var-free)
File: `libs/vkd3d/device.c`, in a fenced `MSFS startup-video diagnostics` block plus two
call sites (`QueryInterface`, `OpenSharedHandle`). When the host process is
`FlightSimulator*.exe`, it appends to:

```
%TEMP%\vkd3d-msfs-video.log
```

(For the Store sandbox `%TEMP%` resolves to the package-local temp dir, which is
writable.) It records every unsupported `QueryInterface` on the D3D12 device (video
interfaces named) and every `OpenSharedHandle`. No env vars, no debug-level change.
Remove by deleting the fenced block and its two call sites — no other files touched.

### How to read it (one launch decides the fix location)
- **`OpenSharedHandle:` lines present** → MSFS is importing the decoded video frame into
  vkd3d's D3D12. If those textures come back wrong/failed, this is fixable **here** in
  vkd3d (OpenSharedHandle / shared-texture path). Send me the log.
- **Only `QueryInterface: ... VideoDevice*` or nothing video-related** → vkd3d is not in
  the video path; the fix is on the **DXVK / Media Foundation** side (below).

## Where the real fix likely belongs (and what to try)
Ordered by effort, gated on what the log shows:
1. **Bump DXVK** in `runtime.lock.json` (currently pinned to 3.0). DXVK's D3D11-video /
   MF interop has improved over releases; a newer build may fix picture-on-Windows.
2. **Force MF software video decode** (avoid the DXVA/D3D11-video path native MF prefers),
   e.g. disabling hardware MFTs so the frame arrives as a plain texture DXVK handles —
   the same effective path that works on Linux.
3. **vkd3d OpenSharedHandle fix** — only if the log shows MSFS sharing the frame into
   D3D12 and it fails. Do **not** implement a D3D12 video decoder; that is out of scope.

## Build & release (CI/CD)
Local Windows toolchain isn't required — CI builds it.
- **Build on every push/PR:** `.github/workflows/artifacts.yml` (already present) builds via
  the Arch-mingw action and uploads `vkd3d-proton-<ref>` as a workflow artifact.
- **Draft release on tags:** `.github/workflows/release.yml` (added) triggers on `v*` tags
  (or manual dispatch), runs `package-release.sh`, and drafts a GitHub release with
  `vkd3d-proton-<version>.tar.zst`. The leading `v` is stripped so the asset name matches
  the launcher's `runtime.lock.json` (`vkd3d-proton-3.0.1.tar.zst`).

To cut a build/release:
```
git tag v3.0.1-msfs1
git push origin v3.0.1-msfs1
```
Then update `msfs2024-vulkan/runtime.lock.json` (`archive`, `archive_sha256`, per-file
hashes) to point at the drafted release once published.

Local build (optional, needs MSVC + widl(StrawberryPerl) + glslangValidator + meson):
```
meson setup --backend vs2022 -Denable_tests=false build-msvc
msbuild build-msvc/vkd3d-proton.sln
```

## Test matrix in MSFS
Capture `%TEMP%\vkd3d-msfs-video.log` for each:
1. Current DLL set (vkd3d d3d12 + DXVK d3d11/dxgi), default launch.
2. Newer DXVK d3d11/dxgi (bump), same vkd3d.
3. With MF hardware decode disabled (software video).
4. `-FastLaunch` on/off (FastLaunch may skip the startup video entirely).
