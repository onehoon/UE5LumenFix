#include "pch.h"

namespace dxgi_proxy {

Config::Config()
{
	char path[MAX_PATH]{};
	GetSystemDirectoryA(path, sizeof(path) - 1);
	dxgi_section.dll_path = std::string{path} + "\\dxgi.dll";
}

}  // namespace dxgi_proxy
