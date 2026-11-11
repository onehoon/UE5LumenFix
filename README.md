# d3d12-proxy
Graphics card spoofing library for DirectX;
May be used for fix UE5 Lumen GI bug on integrated GPUs:
UE5 do not allow 3D textures for GI usage on these GPUs
(if non local GPU memory is zero card treated as integrated)
and Lumen GI automatically disabled in this case.
This proxy allows GPU memory size spoofing which force UE5 treat GPU as dedicated.

## ASI installation (OptiScaler)
* Place `UE5LumenFix.asi` in `OptiScaler/plugins/`
* Enable the existing `[Plugins] LoadAsiPlugins=true` setting in OptiScaler
* No INI or other configuration file is used
* The ASI overwrites `OptiScaler/plugins/UE5LumenFix.log` on each game launch

## Intel ReflexFix ASI
* Place `IntelReflexFix.asi` in `OptiScaler/plugins/` alongside upstream OptiScaler and REFramework.
* Set these existing OptiScaler options; do not add the `-loadlate` suffix:
  ```ini
  [Plugins]
  LoadAsiPlugins=true

  [Spoofing]
  Dxgi=false
  StreamlineSpoofing=true
  DxgiVRAM=auto
  ```
* The ASI only targets `re9.exe` and `pragmata.exe`. It changes the VendorId, DeviceId, and description only for Intel adapter descriptors requested directly by the game executable.
* Test with upstream OptiScaler and REFramework only; do not combine it with OptiPatcher or another GPU-identity ASI.
* Manually compare the Reflex option with and without the ASI in both games, then confirm XeFG and normal launch, gameplay, exit, and relaunch behavior. This ASI has not been hardware-validated.

## Original DXGI proxy fallback
* To use it just place `dxgi.dll` and `dxgi_proxy.ini` next to executable
* Edit `dxgi_proxy.ini` if needed

## UE5 LumenGI on integrated cards problem description
UE5 GPU type (integrated/dedicated) detection algo:
	[d3d11](https://github.com/EpicGames/UnrealEngine/blob/release/Engine/Source/Runtime/Windows/D3D11RHI/Private/Windows/WindowsD3D11Device.cpp#L1102)
	[d3d12](https://github.com/EpicGames/UnrealEngine/blob/release/Engine/Source/Runtime/D3D12RHI/Private/Windows/WindowsD3D12Device.cpp#L817)

and depend on GPU type later UE5 disable some features, like AO, volumentic fog and maybe something else:
	[place 1](https://github.com/EpicGames/UnrealEngine/blob/release/Engine/Source/Runtime/Renderer/Private/DistanceFieldAmbientOcclusion.cpp#L682)
	[place 2](https://github.com/EpicGames/UnrealEngine/blob/release/Engine/Source/Runtime/Renderer/Private/DistanceFieldAmbientOcclusion.cpp#L731)

In my opinion, it's not right - disable some features for all integrated cards
if only old intel cards (line HD 4000) has issues with this features.
