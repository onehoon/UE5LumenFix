#pragma once

namespace dxgi_proxy {

// Fixed configuration: no INI, vendor-ID spoofing, or runtime options.
class Config final
{
public:
	struct Override final
	{
		bool only_igpu{true};
		std::optional<std::wstring> gpu_description;
		std::optional<UINT> gpu_vendor_id;
		std::optional<UINT> gpu_device_id;
		std::optional<SIZE_T> gpu_dedicated_memory_size;
		std::optional<SIZE_T> gpu_non_local_memory_size{static_cast<SIZE_T>(2147483648ULL)};
		std::optional<std::chrono::seconds> disable_after{std::chrono::seconds{60}};
	};

	struct DXGI final
	{
		std::string dll_path;
		bool enable_factory_proxy{true};
		bool enable_adapter_proxy{true};
	};

	Config();

	[[nodiscard]] const DXGI &dxgiSection() const noexcept { return dxgi_section; }
	[[nodiscard]] const Override &overrideSection() const noexcept { return override_section; }

private:
	Override override_section;
	DXGI dxgi_section;
};

}  // namespace dxgi_proxy
