# Work Order — Minimal DXGI Proxy → ASI Conversion

**Repository:** `onehoon/UE5LumenFix`  
**Implementation base/target:** `refactor/asi`  
**Scope:** one small implementation PR after this work order. Codex implements/builds and provides a testable ASI; the user runs the actual game and reviews logs before making the go/no-go decision. The proven `dxgi.dll` proxy remains available as fallback.

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
5. Implement `InitializeASI` to set up the effective hook before returning on the supported early-load path. An ASI reported as loaded does **not** prove that the game reached our hook. **Codex is not expected to prove runtime interception or Lumen behavior without the user's game environment.** Avoid unsafe DXGI/COM initialization under the loader lock, but do not add asynchronous workarounds or modify OptiScaler.

## Logging (always on, no INI)

Place the file beside the ASI, **`OptiScaler/plugins/UE5LumenFix.log`**, resolved relative to the loaded ASI module path, **not** the process working directory. Overwrite it each game launch. If logging cannot open, do not fail game startup.

Log just the useful diagnostics: version/process, `InitializeASI` entered, hook installed or failed (with error), first real `QueryVideoMemoryInfo` call with NodeIndex/group/original Budget, first actual 0→2 GiB override, and 60-second expiry if observed. Do not log every frame/query or build log rotation/telemetry.

## Build, handoff, and user-side validation

**Codex responsibility (implementation PR ready for review):**
- Add the ASI project to the existing Visual Studio solution; preserve the original proxy project. Adjust GitHub Actions minimally so an **x64 Release** ASI artifact is uploaded and PRs targeting `refactor/asi` can be built.
- Document the exact install path and the absence of an INI in `README.md` (keep original proxy usage distinguishable).
- Ensure the code compiles and CI uploads `UE5LumenFix.asi`. Provide the CI artifact link, install instructions, and the location of `UE5LumenFix.log`.
- Include simple, meaningful log events so the user can diagnose real loading/hooking. Do **not** claim that game-level hooking, Lumen activation, or OptiScaler compatibility was verified solely by CI or code inspection.
- **Stop Codex work at a testable build/PR**. Do not wait for or fabricate game-test results; do not introduce speculative fixes before the user's logs are available.

**User responsibility (real-game decision, performed after the build):**
- Download the built `UE5LumenFix.asi`, install it in `OptiScaler/plugins/` for a real UE5/iGPU game, with existing `LoadAsiPlugins=true`.
- Run the game; collect **`OptiScaler/plugins/UE5LumenFix.log`** and, as needed, `OptiScaler.log`. Optionally compare OptiScaler's `winmm.dll` and `dxgi.dll` proxy modes.
- Inspect whether **`InitializeASI` → hook installed → real QueryVideoMemoryInfo call → Budget overridden** occurred; visually compare Lumen behavior against the proven original `dxgi.dll` fix and check game startup/shutdown.
- **Only the user's actual game test and log analysis decide success or failure.** If loading/hooking fails, Lumen is unchanged, or a real crash/conflict appears, the user can abandon the ASI experiment and keep the original DXGI proxy. No automatic abandonment or speculative architecture changes by Codex.

**Acceptance boundary:** CI/build completion means *ready for manual testing*, **not** that ASI compatibility or the Lumen fix has been validated.

## Out of scope

No new game/GPU master fields, bundle merge work, Linux packaging changes, universal injection, per-game configuration, elaborate error recovery, performance framework, or redesign of the original fix. **One focused implementation PR; go/no-go based on the first real-game test.**
