#include <Windows.h>
#include <dxgi1_6.h>
#include <intrin.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
std::atomic<bool> g_initializationStarted{false};

HMODULE g_asiModule = nullptr;
ULONGLONG g_initializeTick = 0;
SYSTEMTIME g_initializeWallClock{};
HANDLE g_logFile = INVALID_HANDLE_VALUE;
SRWLOCK g_logLock = SRWLOCK_INIT;
ULONGLONG g_callSequence = 0;
ULONGLONG g_lastCallTick = 0;

struct CallerInfo {
	bool resolved = false;
	bool isGame = false;
	char moduleName[MAX_PATH] = "unknown";
};

void formatWallClock(const SYSTEMTIME &time, char *output, size_t outputCount) noexcept
{
	_snprintf_s(
	    output,
	    outputCount,
	    _TRUNCATE,
	    "%04u-%02u-%02u %02u:%02u:%02u.%03u",
	    static_cast<unsigned int>(time.wYear),
	    static_cast<unsigned int>(time.wMonth),
	    static_cast<unsigned int>(time.wDay),
	    static_cast<unsigned int>(time.wHour),
	    static_cast<unsigned int>(time.wMinute),
	    static_cast<unsigned int>(time.wSecond),
	    static_cast<unsigned int>(time.wMilliseconds));
}

void writeLogLineLocked(const SYSTEMTIME &wallClock, ULONGLONG tick, const char *message) noexcept
{
	if(g_logFile == INVALID_HANDLE_VALUE) {
		return;
	}

	char line[2048]{};
	const ULONGLONG elapsed = tick >= g_initializeTick ? tick - g_initializeTick : 0;
	_snprintf_s(
	    line,
	    sizeof(line),
	    _TRUNCATE,
	    "[%04u-%02u-%02u %02u:%02u:%02u.%03u] +%llums tid=%lu %s\r\n",
	    static_cast<unsigned int>(wallClock.wYear),
	    static_cast<unsigned int>(wallClock.wMonth),
	    static_cast<unsigned int>(wallClock.wDay),
	    static_cast<unsigned int>(wallClock.wHour),
	    static_cast<unsigned int>(wallClock.wMinute),
	    static_cast<unsigned int>(wallClock.wSecond),
	    static_cast<unsigned int>(wallClock.wMilliseconds),
	    static_cast<unsigned long long>(elapsed),
	    static_cast<unsigned long>(GetCurrentThreadId()),
	    message);

	const DWORD length = static_cast<DWORD>(std::strlen(line));
	DWORD totalWritten = 0;
	while(totalWritten < length) {
		DWORD written = 0;
		if(!WriteFile(g_logFile, line + totalWritten, length - totalWritten, &written, nullptr) || written == 0) {
			break;
		}
		totalWritten += written;
	}
}

void logEvent(const char *format, ...) noexcept
{
	if(g_logFile == INVALID_HANDLE_VALUE) {
		return;
	}

	char message[1200]{};
	va_list arguments;
	va_start(arguments, format);
	_vsnprintf_s(message, sizeof(message), _TRUNCATE, format, arguments);
	va_end(arguments);

	AcquireSRWLockExclusive(&g_logLock);
	SYSTEMTIME wallClock{};
	GetLocalTime(&wallClock);
	writeLogLineLocked(wallClock, GetTickCount64(), message);
	ReleaseSRWLockExclusive(&g_logLock);
}

bool wideToUtf8(const wchar_t *input, char *output, size_t outputCount) noexcept
{
	if(input == nullptr || output == nullptr || outputCount == 0) {
		return false;
	}

	const int converted = WideCharToMultiByte(
	    CP_UTF8,
	    0,
	    input,
	    -1,
	    output,
	    static_cast<int>(outputCount),
	    nullptr,
	    nullptr);
	if(converted == 0) {
		output[0] = '\0';
		return false;
	}
	return true;
}

bool getExecutableName(wchar_t *name, size_t nameCount) noexcept
{
	wchar_t executablePath[MAX_PATH]{};
	const DWORD length = GetModuleFileNameW(nullptr, executablePath, _countof(executablePath));
	if(length == 0 || length >= _countof(executablePath)) {
		return false;
	}

	const wchar_t *fileName = wcsrchr(executablePath, L'\\');
	const wchar_t *forwardSlash = wcsrchr(executablePath, L'/');
	if(forwardSlash != nullptr && (fileName == nullptr || forwardSlash > fileName)) {
		fileName = forwardSlash;
	}
	fileName = fileName != nullptr ? fileName + 1 : executablePath;
	return wcscpy_s(name, nameCount, fileName) == 0;
}

bool isTargetGame() noexcept
{
	wchar_t executableName[MAX_PATH]{};
	if(!getExecutableName(executableName, _countof(executableName))) {
		return false;
	}
	return _wcsicmp(executableName, L"re9.exe") == 0 || _wcsicmp(executableName, L"pragmata.exe") == 0;
}

CallerInfo resolveCaller(void *callerAddress) noexcept
{
	CallerInfo caller{};
	HMODULE callerModule = nullptr;
	const auto moduleAddress = reinterpret_cast<LPCWSTR>(callerAddress);
	if(!GetModuleHandleExW(
	       GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	       moduleAddress,
	       &callerModule)) {
		return caller;
	}

	caller.resolved = true;
	caller.isGame = callerModule == GetModuleHandleW(nullptr);
	wchar_t modulePath[MAX_PATH]{};
	const DWORD length = GetModuleFileNameW(callerModule, modulePath, _countof(modulePath));
	if(length == 0 || length >= _countof(modulePath)) {
		return caller;
	}

	const wchar_t *fileName = wcsrchr(modulePath, L'\\');
	const wchar_t *forwardSlash = wcsrchr(modulePath, L'/');
	if(forwardSlash != nullptr && (fileName == nullptr || forwardSlash > fileName)) {
		fileName = forwardSlash;
	}
	fileName = fileName != nullptr ? fileName + 1 : modulePath;
	wideToUtf8(fileName, caller.moduleName, _countof(caller.moduleName));
	return caller;
}

bool openLogFile() noexcept
{
	if(g_asiModule == nullptr) {
		return false;
	}

	wchar_t modulePath[32768]{};
	const DWORD length = GetModuleFileNameW(g_asiModule, modulePath, _countof(modulePath));
	if(length == 0 || length >= _countof(modulePath)) {
		return false;
	}

	wchar_t *separator = wcsrchr(modulePath, L'\\');
	wchar_t *forwardSlash = wcsrchr(modulePath, L'/');
	if(forwardSlash != nullptr && (separator == nullptr || forwardSlash > separator)) {
		separator = forwardSlash;
	}
	if(separator == nullptr) {
		return false;
	}
	separator[1] = L'\0';
	if(wcscat_s(modulePath, _countof(modulePath), L"IntelReflexFix.log") != 0) {
		return false;
	}

	g_logFile = CreateFileW(
	    modulePath,
	    FILE_APPEND_DATA,
	    FILE_SHARE_READ | FILE_SHARE_WRITE,
	    nullptr,
	    CREATE_ALWAYS,
	    FILE_ATTRIBUTE_NORMAL,
	    nullptr);
	return g_logFile != INVALID_HANDLE_VALUE;
}

void logDescriptorCall(
	const char *method,
	void *callerAddress,
	const CallerInfo &caller,
	HRESULT result,
	bool hasOriginalDescriptor,
	UINT originalVendorId,
	UINT originalDeviceId,
	bool rewritten,
	const char *reason) noexcept
{
	if(g_logFile == INVALID_HANDLE_VALUE) {
		return;
	}

	AcquireSRWLockExclusive(&g_logLock);
	const ULONGLONG tick = GetTickCount64();
	const ULONGLONG sequence = ++g_callSequence;
	const char *deltaText = "first";
	char deltaBuffer[48]{};
	if(g_lastCallTick != 0) {
		_snprintf_s(
		    deltaBuffer,
		    sizeof(deltaBuffer),
		    _TRUNCATE,
		    "%llums",
		    static_cast<unsigned long long>(tick - g_lastCallTick));
		deltaText = deltaBuffer;
	}
	g_lastCallTick = tick;

	char originalIds[32] = "n/a";
	if(hasOriginalDescriptor) {
		_snprintf_s(
		    originalIds,
		    sizeof(originalIds),
		    _TRUNCATE,
		    "%04X:%04X",
		    originalVendorId,
		    originalDeviceId);
	}

	char resultingIds[64]{};
	if(rewritten) {
		_snprintf_s(
		    resultingIds,
		    sizeof(resultingIds),
		    _TRUNCATE,
		    " -> %04X:%04X",
		    kNvidiaVendorId,
		    kNvidiaDeviceId);
	}

	char message[1200]{};
	_snprintf_s(
	    message,
	    sizeof(message),
	    _TRUNCATE,
	    "seq=%llu delta=%s method=%s hr=0x%08lX caller=%s caller_module=%s return=%p original=%s rewritten=%s reason=%s%s",
	    static_cast<unsigned long long>(sequence),
	    deltaText,
	    method,
	    static_cast<unsigned long>(result),
	    caller.resolved ? (caller.isGame ? "game" : "other") : "unknown",
	    caller.moduleName,
	    callerAddress,
	    originalIds,
	    rewritten ? "yes" : "no",
	    reason,
	    resultingIds);

	SYSTEMTIME wallClock{};
	GetLocalTime(&wallClock);
	writeLogLineLocked(wallClock, tick, message);
	ReleaseSRWLockExclusive(&g_logLock);
}

template <typename Descriptor>
void rewriteIntelDescriptor(Descriptor *descriptor) noexcept
{
	descriptor->VendorId = kNvidiaVendorId;
	descriptor->DeviceId = kNvidiaDeviceId;
	wcscpy_s(descriptor->Description, _countof(descriptor->Description), kSpoofedDescription);
}

template <typename Adapter, typename Descriptor, typename Function>
HRESULT invokeDescriptorHook(
	const char *method,
	Adapter *adapter,
	Descriptor *descriptor,
	Function original,
	void *callerAddress)
{
	if(original == nullptr) {
		const CallerInfo caller = resolveCaller(callerAddress);
		logDescriptorCall(method, callerAddress, caller, E_UNEXPECTED, false, 0, 0, false, "original_missing");
		return E_UNEXPECTED;
	}

	const HRESULT result = original(adapter, descriptor);
	const CallerInfo caller = resolveCaller(callerAddress);
	const bool hooksActive = g_hooksActive.load(std::memory_order_acquire);
	const bool hasOriginalDescriptor = SUCCEEDED(result) && descriptor != nullptr;
	const UINT originalVendorId = hasOriginalDescriptor ? descriptor->VendorId : 0;
	const UINT originalDeviceId = hasOriginalDescriptor ? descriptor->DeviceId : 0;
	bool rewritten = false;
	const char *reason = "call_failed";

	if(FAILED(result)) {
		reason = "call_failed";
	} else if(descriptor == nullptr) {
		reason = "null_descriptor";
	} else if(!hooksActive) {
		reason = "hook_inactive";
	} else if(originalVendorId != kIntelVendorId) {
		reason = "non_intel";
	} else if(!caller.isGame) {
		reason = caller.resolved ? "non_game_caller" : "caller_unresolved";
	} else {
		rewriteIntelDescriptor(descriptor);
		rewritten = true;
		reason = "eligible_game_intel_adapter";
	}

	logDescriptorCall(
	    method,
	    callerAddress,
	    caller,
	    result,
	    hasOriginalDescriptor,
	    originalVendorId,
	    originalDeviceId,
	    rewritten,
	    reason);
	return result;
}

HRESULT STDMETHODCALLTYPE hookGetDesc(IDXGIAdapter *adapter, DXGI_ADAPTER_DESC *descriptor)
{
	void *callerAddress = _ReturnAddress();
	return invokeDescriptorHook(
	    "GetDesc",
	    adapter,
	    descriptor,
	    g_originalGetDesc.load(std::memory_order_acquire),
	    callerAddress);
}

HRESULT STDMETHODCALLTYPE hookGetDesc1(IDXGIAdapter1 *adapter, DXGI_ADAPTER_DESC1 *descriptor)
{
	void *callerAddress = _ReturnAddress();
	return invokeDescriptorHook(
	    "GetDesc1",
	    adapter,
	    descriptor,
	    g_originalGetDesc1.load(std::memory_order_acquire),
	    callerAddress);
}

HRESULT STDMETHODCALLTYPE hookGetDesc2(IDXGIAdapter2 *adapter, DXGI_ADAPTER_DESC2 *descriptor)
{
	void *callerAddress = _ReturnAddress();
	return invokeDescriptorHook(
	    "GetDesc2",
	    adapter,
	    descriptor,
	    g_originalGetDesc2.load(std::memory_order_acquire),
	    callerAddress);
}

HRESULT STDMETHODCALLTYPE hookGetDesc3(IDXGIAdapter4 *adapter, DXGI_ADAPTER_DESC3 *descriptor)
{
	void *callerAddress = _ReturnAddress();
	return invokeDescriptorHook(
	    "GetDesc3",
	    adapter,
	    descriptor,
	    g_originalGetDesc3.load(std::memory_order_acquire),
	    callerAddress);
}

template <typename Function>
bool installVtableHook(void **slot, Function hook, std::atomic<Function> &original, HRESULT *status) noexcept
{
	if(status != nullptr) {
		*status = E_FAIL;
	}
	if(slot == nullptr) {
		if(status != nullptr) {
			*status = E_POINTER;
		}
		return false;
	}

	const auto hookAddress = reinterpret_cast<void *>(hook);
	const auto current = *slot;
	if(current == hookAddress) {
		const bool installed = original.load(std::memory_order_acquire) != nullptr;
		if(status != nullptr) {
			*status = installed ? S_OK : E_UNEXPECTED;
		}
		return installed;
	}
	if(current == nullptr) {
		if(status != nullptr) {
			*status = E_POINTER;
		}
		return false;
	}

	original.store(reinterpret_cast<Function>(current), std::memory_order_release);

	DWORD oldProtection = 0;
	if(!VirtualProtect(slot, sizeof(void *), PAGE_EXECUTE_READWRITE, &oldProtection)) {
		const DWORD error = GetLastError();
		original.store(nullptr, std::memory_order_release);
		if(status != nullptr) {
			*status = HRESULT_FROM_WIN32(error != ERROR_SUCCESS ? error : ERROR_GEN_FAILURE);
		}
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
		if(status != nullptr) {
			*status = HRESULT_FROM_WIN32(ERROR_RETRY);
		}
		return false;
	}
	if(status != nullptr) {
		*status = S_OK;
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

void logHookInstall(UINT adapterIndex, const char *method, bool installed, HRESULT status, const char *detail) noexcept
{
	logEvent(
	    "event=hook_install adapter=%u method=%s installed=%s hr=0x%08lX detail=%s",
	    adapterIndex,
	    method,
	    installed ? "yes" : "no",
	    static_cast<unsigned long>(status),
	    detail);
}

bool installAdapterHooks(IDXGIAdapter4 *adapter, UINT adapterIndex) noexcept
{
	auto **vtable = *reinterpret_cast<void ***>(adapter);
	HRESULT status = S_OK;
	const bool getDescInstalled =
	    installVtableHook(&vtable[kGetDescVtableIndex], hookGetDesc, g_originalGetDesc, &status);
	logHookInstall(adapterIndex, "GetDesc", getDescInstalled, status, getDescInstalled ? "installed" : "failed");

	HRESULT getDesc1Status = E_ABORT;
	const bool getDesc1Installed = getDescInstalled &&
	                               installVtableHook(
	                                   &vtable[kGetDesc1VtableIndex], hookGetDesc1, g_originalGetDesc1, &getDesc1Status);
	logHookInstall(
	    adapterIndex,
	    "GetDesc1",
	    getDesc1Installed,
	    getDesc1Status,
	    getDescInstalled ? (getDesc1Installed ? "installed" : "failed") : "skipped_after_previous_failure");

	HRESULT getDesc2Status = E_ABORT;
	const bool getDesc2Installed = getDesc1Installed &&
	                               installVtableHook(
	                                   &vtable[kGetDesc2VtableIndex], hookGetDesc2, g_originalGetDesc2, &getDesc2Status);
	logHookInstall(
	    adapterIndex,
	    "GetDesc2",
	    getDesc2Installed,
	    getDesc2Status,
	    getDesc1Installed ? (getDesc2Installed ? "installed" : "failed") : "skipped_after_previous_failure");

	HRESULT getDesc3Status = E_ABORT;
	const bool getDesc3Installed = getDesc2Installed &&
	                               installVtableHook(
	                                   &vtable[kGetDesc3VtableIndex], hookGetDesc3, g_originalGetDesc3, &getDesc3Status);
	logHookInstall(
	    adapterIndex,
	    "GetDesc3",
	    getDesc3Installed,
	    getDesc3Status,
	    getDesc2Installed ? (getDesc3Installed ? "installed" : "failed") : "skipped_after_previous_failure");

	if(getDescInstalled && getDesc1Installed && getDesc2Installed && getDesc3Installed) {
		g_hooksActive.store(true, std::memory_order_release);
		logEvent("event=hooks_final active=yes adapter=%u", adapterIndex);
		return true;
	}

	removeVtableHook(&vtable[kGetDescVtableIndex], hookGetDesc, g_originalGetDesc);
	removeVtableHook(&vtable[kGetDesc1VtableIndex], hookGetDesc1, g_originalGetDesc1);
	removeVtableHook(&vtable[kGetDesc2VtableIndex], hookGetDesc2, g_originalGetDesc2);
	removeVtableHook(&vtable[kGetDesc3VtableIndex], hookGetDesc3, g_originalGetDesc3);
	logEvent("event=hooks_final active=no adapter=%u reason=installation_failed", adapterIndex);
	return false;
}

bool installIntelAdapterHooks() noexcept
{
	IDXGIFactory1 *factory = nullptr;
	const HRESULT factoryResult = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
	logEvent("event=create_dxgi_factory1 hr=0x%08lX", static_cast<unsigned long>(factoryResult));
	if(FAILED(factoryResult)) {
		logEvent("event=hooks_final active=no reason=create_dxgi_factory1_failed");
		return false;
	}

	bool installed = false;
	for(UINT index = 0;; ++index) {
		IDXGIAdapter1 *adapter = nullptr;
		const HRESULT enumResult = factory->EnumAdapters1(index, &adapter);
		const char *enumDetail = enumResult == DXGI_ERROR_NOT_FOUND ? "end_of_list" : (SUCCEEDED(enumResult) ? "adapter" : "failed");
		logEvent(
		    "event=enum_adapters1 index=%u hr=0x%08lX detail=%s",
		    index,
		    static_cast<unsigned long>(enumResult),
		    enumDetail);
		if(enumResult == DXGI_ERROR_NOT_FOUND) {
			break;
		}
		if(FAILED(enumResult)) {
			break;
		}

		DXGI_ADAPTER_DESC1 descriptor{};
		const HRESULT descResult = adapter->GetDesc1(&descriptor);
		char adapterIds[32] = "n/a";
		if(SUCCEEDED(descResult)) {
			_snprintf_s(
			    adapterIds,
			    sizeof(adapterIds),
			    _TRUNCATE,
			    "%04X:%04X",
			    descriptor.VendorId,
			    descriptor.DeviceId);
		}
		logEvent(
		    "event=adapter_get_desc1 index=%u hr=0x%08lX ids=%s",
		    index,
		    static_cast<unsigned long>(descResult),
		    adapterIds);
		if(SUCCEEDED(descResult) && descriptor.VendorId == kIntelVendorId) {
			IDXGIAdapter4 *adapter4 = nullptr;
			const HRESULT queryResult = adapter->QueryInterface(IID_PPV_ARGS(&adapter4));
			logEvent(
			    "event=query_interface_adapter4 index=%u hr=0x%08lX",
			    index,
			    static_cast<unsigned long>(queryResult));
			if(SUCCEEDED(queryResult)) {
				installed = installAdapterHooks(adapter4, index);
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
	if(!installed) {
		logEvent("event=hooks_final active=no reason=no_usable_intel_adapter");
	}
	return installed;
}

DWORD WINAPI initializeWorker(LPVOID)
{
	const ULONGLONG workerStartTick = GetTickCount64();
	if(!openLogFile()) {
		OutputDebugStringA("[IntelReflexFix] Could not open IntelReflexFix.log; continuing without file logging.\n");
	}

	wchar_t executableNameWide[MAX_PATH]{};
	char executableName[MAX_PATH * 4] = "unknown";
	if(getExecutableName(executableNameWide, _countof(executableNameWide))) {
		wideToUtf8(executableNameWide, executableName, _countof(executableName));
	}
	char initializeWallClock[40] = "unknown";
	formatWallClock(g_initializeWallClock, initializeWallClock, _countof(initializeWallClock));
	const ULONGLONG schedulingDelay = workerStartTick >= g_initializeTick ? workerStartTick - g_initializeTick : 0;
	logEvent(
	    "event=worker_start init_entry_wall=%s worker_start_delay=%llums exe=%s",
	    initializeWallClock,
	    static_cast<unsigned long long>(schedulingDelay),
	    executableName);
	logEvent("event=asi_loaded exe=%s", executableName);

	const bool targetGame = isTargetGame();
	logEvent("event=target_check passed=%s exe=%s", targetGame ? "yes" : "no", executableName);
	bool installed = false;
	if(targetGame) {
		installed = installIntelAdapterHooks();
	} else {
		logEvent("event=hooks_final active=no reason=unsupported_executable");
	}
	logEvent("event=worker_complete hooks_active=%s", installed ? "yes" : "no");
	return 0;
}

}  // namespace

extern "C" __declspec(dllexport) void InitializeASI()
{
	const ULONGLONG entryTick = GetTickCount64();
	SYSTEMTIME entryWallClock{};
	GetLocalTime(&entryWallClock);
	bool expected = false;
	if(!g_initializationStarted.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
		return;
	}

	g_initializeTick = entryTick;
	g_initializeWallClock = entryWallClock;
	HANDLE worker = CreateThread(nullptr, 0, initializeWorker, nullptr, 0, nullptr);
	if(worker == nullptr) {
		OutputDebugStringA("[IntelReflexFix] CreateThread failed; DXGI hooks were not installed.\n");
		return;
	}
	CloseHandle(worker);
}

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID)
{
	if(reason == DLL_PROCESS_ATTACH) {
		g_asiModule = module;
	}
	return TRUE;
}
