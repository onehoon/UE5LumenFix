# Work Order — Minimal DXGI Proxy → ASI Conversion

**Repository:** `onehoon/UE5LumenFix`  
**Implementation base/target:** `refactor/asi`  
**Scope:** one small implementation PR after this work order. First confirm early loading and the real DXGI call path. If that fails, stop and keep the proven `dxgi.dll` proxy.

## Goal

Free the game's root `dxgi.dll` proxy name by shipping **`UE5LumenFix.asi`**, loaded through OptiScaler's existing ASI loader. This is a packaging/loading experiment, **not** a rewrite or feature expansion. The original proxy is already a working fallback.

## Constraints

- Add a root **`asi/`** implementation directory. Leave **`dxgi-proxy/`** intact for direct comparison and rollback; do not empty the branch or duplicate the entire proxy project.
- Build **Windows x64 Release `UE5LumenFix.asi`**. Export `InitializeASI` for OptiScaler's `LoadAsiPlugins()`. Keep `DllMain` minimal. Do not implement a separate ASI loader, game launcher, injection mechanism, background worker, or deferred initialization framework.
- **No `dxgi_proxy.ini`, replacement INI, runtime options, user UI, or GPU vendor/model detection.** OptiClick already selects which games/iGPU bundles receive the ASI. `[Plugins] LoadAsiPlugins=true` is already handled by the distribution policy; do not add installer or OptiScaler changes in this task.
- **Do not modify** OptiClick, OptiClick-Linux, DataOps, Game Master, or GPU bundle policy in this PR.

## Minimal implementation

1. Reuse only the proven behavior from `dxgi-proxy/dxgi_proxy_adapter.cpp`. Replace the DXGI factory/adapter **proxy objects** with the smallest workable hook of the **actual** `IDXGIAdapter3::QueryVideoMemoryInfo` method. Prefer one simple, conventional detour; do not build an abstraction layer or multi-route fallback architecture.
2. On each hooked call, **call the original method first**. Override **only** `DXGI_QUERY_VIDEO_MEMORY_INFO::Budget` to **`2147483648ULL` (2 GiB)** when all are true:
   - the original call succeeded, output is non-null, and `NodeIndex == 0`;
   - `MemorySegmentGroup == DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL`;
   - original **`Budget == 0`**;
   - the existing **60-second temporary override** is active.
3. Preserve every other field and return code. If the Budget is nonzero (including a dGPU), return the original result without modification. **Do not perform an additional `isIntegrated()` / probe call.** Do not spoof VRAM descriptions, vendor/device IDs, `GetDesc*`, or LOCAL budgets.
4. Preserve the original temporary-override intent without additional state machinery. Because there is no longer an adapter proxy constructor, a single 60-second window starting at the **first eligible zero-Budget call** is acceptable. After expiry simply pass through; no hot unhook or timer thread.
5. `InitializeASI` must install the effective hook before returning on the supported early-load path. An ASI reported as loaded does **not** prove that the game reached our hook. If the actual UE5 query cannot be intercepted in time, or hooking conflicts with OptiScaler, **stop this experiment** rather than adding asynchronous workarounds or modifying OptiScaler. In particular, avoid unsafe DXGI/COM initialization under the loader lock.

## Logging (always on, no INI)

Place the file beside the ASI, **`OptiScaler/plugins/UE5LumenFix.log`**, resolved relative to the loaded ASI module path, **not** the process working directory. Overwrite it each game launch. If logging cannot open, do not fail game startup.

Log just the useful diagnostics: version/process, `InitializeASI` entered, hook installed or failed (with error), first real `QueryVideoMemoryInfo` call with NodeIndex/group/original Budget, first actual 0→2 GiB override, and 60-second expiry if observed. Do not log every frame/query or build log rotation/telemetry.

## Build and verification

- Add the ASI project to the existing Visual Studio solution; preserve the original proxy project. Adjust GitHub Actions minimally so an **x64 Release** ASI artifact is uploaded and PRs targeting `refactor/asi` can be built.
- Document the exact install path and the absence of an INI in `README.md` (keep original proxy usage distinguishable).
- Test with OptiScaler using its existing `LoadAsiPlugins=true`, at least one UE5/iGPU game, and ideally both `winmm.dll` and `dxgi.dll` OptiScaler proxy modes.
- Verify from the log that **`InitializeASI` → hook installed → game's real QueryVideoMemoryInfo call → Budget overridden** occurs in that order. Then visually compare Lumen behavior against the proven original `dxgi.dll` fix and confirm normal game startup/shutdown.
- If loading succeeds but no relevant call is intercepted, Lumen is unchanged, or a real crash/hook conflict occurs, **do not add complex defenses**. Report the finding and keep the original DXGI proxy approach.

## Out of scope

No new game/GPU master fields, bundle merge work, Linux packaging changes, universal injection, per-game configuration, elaborate error recovery, performance framework, or redesign of the original fix. **One focused implementation PR; go/no-go based on the first real-game test.**
