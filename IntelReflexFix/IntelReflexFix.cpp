#include <Windows.h>
#include <dxgi1_6.h>
#include <intrin.h>

#include <atomic>
#include <cwchar>

namespace {

constexpr UINT kGetDescVtableIndex = 8;
constexpr UINT kGetDesc1VtableIndex = 10;
constexpr UINT kGetDesc2VtableIndex = 11;
constexpr UINT kGetDesc3VtableIndex = 18;
constexpr UINT kIntelVendorId = 0x8086;
constexpr UINT kNvidiaVendorId = 0x10DE;
constexpr UINT kNvidiaDeviceId = 0x2684;
constexpr wchar_t kSpoofedDescription[] = L"NVIDIA GeForce RTX 4090";

using GetDescFn = HRESULT(STDMETHODCALLTYPE *)(IDXGIAdapter *, DXGI_ADAPTER_DESC *);
using GetDesc1Fn = HRESULT(STDMETHODCALLTYPE *)(IDXGIAdapter1 *, DXGI_ADAPTER_DESC1 *);
using GetDesc2Fn = HRESULT(STDMETHODCALLTYPE *)(IDXGIAdapter2 *, DXGI_ADAPTER_DESC2 *);
using GetDesc3Fn = HRESULT(STDMETHODCALLTYPE *)(IDXGIAdapter4 *, DXGI_ADAPTER_DESC3 *);

std::atomic<GetDescFn> g_originalGetDesc{nullptr};
std::atomic<GetDesc1Fn> g_originalGetDesc1{nullptr};
std::atomic<GetDesc2Fn> g_originalGetDesc2{nullptr};
std::atomic<GetDesc3Fn> g_originalGetDesc3{nullptr};
std::atomic<bool> g_hooksActive{false};

template <typename Descriptor>
void rewriteIntelDescriptorForGame(void *callerAddress, Descriptor *descriptor) noexcept
{
	if(descriptor == nullptr || descriptor->VendorId != kIntelVendorId) {
		return;
	}

	HMODULE callerModule = nullptr;
	const auto moduleAddress = reinterpret_cast<LPCWSTR>(callerAddress);
	if(!GetModuleHandleExW(
	       GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	       moduleAddress,
	       &callerModule) ||
	   callerModule != GetModuleHandleW(nullptr)) {
		return;
	}

	descriptor->VendorId = kNvidiaVendorId;
	descriptor->DeviceId = kNvidiaDeviceId;
	wcscpy_s(descriptor->Description, _countof(descriptor->Description), kSpoofedDescription);
}

HRESULT STDMETHODCALLTYPE hookGetDesc(IDXGIAdapter *adapter, DXGI_ADAPTER_DESC *descriptor)
{
	void *callerAddress = _ReturnAddress();
	const auto original = g_originalGetDesc.load(std::memory_order_acquire);
	if(original == nullptr) {
		return E_UNEXPECTED;
	}

	const HRESULT result = original(adapter, descriptor);
	if(SUCCEEDED(result) && g_hooksActive.load(std::memory_order_acquire)) {
		rewriteIntelDescriptorForGame(callerAddress, descriptor);
	}
	return result;
}

HRESULT STDMETHODCALLTYPE hookGetDesc1(IDXGIAdapter1 *adapter, DXGI_ADAPTER_DESC1 *descriptor)
{
	void *callerAddress = _ReturnAddress();
	const auto original = g_originalGetDesc1.load(std::memory_order_acquire);
	if(original == nullptr) {
		return E_UNEXPECTED;
	}

	const HRESULT result = original(adapter, descriptor);
	if(SUCCEEDED(result) && g_hooksActive.load(std::memory_order_acquire)) {
		rewriteIntelDescriptorForGame(callerAddress, descriptor);
	}
	return result;
}

HRESULT STDMETHODCALLTYPE hookGetDesc2(IDXGIAdapter2 *adapter, DXGI_ADAPTER_DESC2 *descriptor)
{
	void *callerAddress = _ReturnAddress();
	const auto original = g_originalGetDesc2.load(std::memory_order_acquire);
	if(original == nullptr) {
		return E_UNEXPECTED;
	}

	const HRESULT result = original(adapter, descriptor);
	if(SUCCEEDED(result) && g_hooksActive.load(std::memory_order_acquire)) {
		rewriteIntelDescriptorForGame(callerAddress, descriptor);
	}
	return result;
}

HRESULT STDMETHODCALLTYPE hookGetDesc3(IDXGIAdapter4 *adapter, DXGI_ADAPTER_DESC3 *descriptor)
{
	void *callerAddress = _ReturnAddress();
	const auto original = g_originalGetDesc3.load(std::memory_order_acquire);
	if(original == nullptr) {
		return E_UNEXPECTED;
	}

	const HRESULT result = original(adapter, descriptor);
	if(SUCCEEDED(result) && g_hooksActive.load(std::memory_order_acquire)) {
		rewriteIntelDescriptorForGame(callerAddress, descriptor);
	}
	return result;
}

template <typename Function>
bool installVtableHook(void **slot, Function hook, std::atomic<Function> &original) noexcept
{
	const auto hookAddress = reinterpret_cast<void *>(hook);
	const auto current = *slot;
	if(current == hookAddress) {
		return original.load(std::memory_order_acquire) != nullptr;
	}
	if(current == nullptr) {
		return false;
	}

	original.store(reinterpret_cast<Function>(current), std::memory_order_release);

	DWORD oldProtection = 0;
	if(!VirtualProtect(slot, sizeof(void *), PAGE_EXECUTE_READWRITE, &oldProtection)) {
		original.store(nullptr, std::memory_order_release);
		return false;
	}

	const auto previous = InterlockedCompareExchangePointer(
	    reinterpret_cast<void *volatile *>(slot),
	    hookAddress,
	    current);
	DWORD ignoredProtection = 0;
	VirtualProtect(slot, sizeof(void *), oldProtection, &ignoredProtection);

	if(previous != current) {
		original.store(nullptr, std::memory_order_release);
		return false;
	}
	return true;
}

template <typename Function>
void removeVtableHook(void **slot, Function hook, const std::atomic<Function> &original) noexcept
{
	const auto originalFunction = original.load(std::memory_order_acquire);
	if(originalFunction == nullptr) {
		return;
	}

	DWORD oldProtection = 0;
	if(!VirtualProtect(slot, sizeof(void *), PAGE_EXECUTE_READWRITE, &oldProtection)) {
		return;
	}

	InterlockedCompareExchangePointer(
	    reinterpret_cast<void *volatile *>(slot),
	    reinterpret_cast<void *>(originalFunction),
	    reinterpret_cast<void *>(hook));
	DWORD ignoredProtection = 0;
	VirtualProtect(slot, sizeof(void *), oldProtection, &ignoredProtection);
}

bool installAdapterHooks(IDXGIAdapter4 *adapter) noexcept
{
	auto **vtable = *reinterpret_cast<void ***>(adapter);
	const bool getDescInstalled =
	    installVtableHook(&vtable[kGetDescVtableIndex], hookGetDesc, g_originalGetDesc);
	const bool getDesc1Installed =
	    getDescInstalled && installVtableHook(&vtable[kGetDesc1VtableIndex], hookGetDesc1, g_originalGetDesc1);
	const bool getDesc2Installed =
	    getDesc1Installed && installVtableHook(&vtable[kGetDesc2VtableIndex], hookGetDesc2, g_originalGetDesc2);
	const bool getDesc3Installed =
	    getDesc2Installed && installVtableHook(&vtable[kGetDesc3VtableIndex], hookGetDesc3, g_originalGetDesc3);

	if(getDescInstalled && getDesc1Installed && getDesc2Installed && getDesc3Installed) {
		g_hooksActive.store(true, std::memory_order_release);
		return true;
	}

	removeVtableHook(&vtable[kGetDescVtableIndex], hookGetDesc, g_originalGetDesc);
	removeVtableHook(&vtable[kGetDesc1VtableIndex], hookGetDesc1, g_originalGetDesc1);
	removeVtableHook(&vtable[kGetDesc2VtableIndex], hookGetDesc2, g_originalGetDesc2);
	removeVtableHook(&vtable[kGetDesc3VtableIndex], hookGetDesc3, g_originalGetDesc3);
	return false;
}

bool isTargetGame() noexcept
{
	wchar_t executablePath[MAX_PATH]{};
	const DWORD length = GetModuleFileNameW(nullptr, executablePath, _countof(executablePath));
	if(length == 0 || length >= _countof(executablePath)) {
		return false;
	}

	const wchar_t *fileName = wcsrchr(executablePath, L'\\');
	fileName = fileName != nullptr ? fileName + 1 : executablePath;
	return _wcsicmp(fileName, L"re9.exe") == 0 || _wcsicmp(fileName, L"pragmata.exe") == 0;
}

bool installIntelAdapterHooks() noexcept
{
	IDXGIFactory1 *factory = nullptr;
	if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
		return false;
	}

	bool installed = false;
	for(UINT index = 0;; ++index) {
		IDXGIAdapter1 *adapter = nullptr;
		const HRESULT enumResult = factory->EnumAdapters1(index, &adapter);
		if(enumResult == DXGI_ERROR_NOT_FOUND) {
			break;
		}
		if(FAILED(enumResult)) {
			break;
		}

		DXGI_ADAPTER_DESC1 descriptor{};
		const HRESULT descResult = adapter->GetDesc1(&descriptor);
		if(SUCCEEDED(descResult) && descriptor.VendorId == kIntelVendorId) {
			IDXGIAdapter4 *adapter4 = nullptr;
			if(SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&adapter4)))) {
				installed = installAdapterHooks(adapter4);
				adapter4->Release();
				adapter->Release();
				if(installed) {
					break;
				}
				continue;
			}
		}

		adapter->Release();
	}

	factory->Release();
	return installed;
}

}  // namespace

extern "C" __declspec(dllexport) void InitializeASI()
{
	if(isTargetGame()) {
		installIntelAdapterHooks();
	}
}
