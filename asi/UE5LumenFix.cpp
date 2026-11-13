#include <Windows.h>
#include <dxgi1_6.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

constexpr char kVersion[] = "1.0.0";
// IUnknown (3), IDXGIObject (4), IDXGIAdapter (3), Adapter1/2 (1 each), then two Adapter3 methods.
constexpr UINT kQueryVideoMemoryInfoVtableIndex = 14;
constexpr ULONGLONG kOverrideDurationMs = 60'000;
constexpr ULONGLONG kOverrideNotStarted = static_cast<ULONGLONG>(-1);
constexpr UINT64 kNonLocalBudget = 2'147'483'648ULL;

using QueryVideoMemoryInfoFn = HRESULT(STDMETHODCALLTYPE *)(IDXGIAdapter3 *,
                                                            UINT,
                                                            DXGI_MEMORY_SEGMENT_GROUP,
                                                            DXGI_QUERY_VIDEO_MEMORY_INFO *);

HRESULT STDMETHODCALLTYPE hookQueryVideoMemoryInfo(
    IDXGIAdapter3 *adapter,
    UINT nodeIndex,
    DXGI_MEMORY_SEGMENT_GROUP group,
    DXGI_QUERY_VIDEO_MEMORY_INFO *info);

HMODULE g_module = nullptr;
std::FILE *g_log = nullptr;
SRWLOCK g_logLock = SRWLOCK_INIT;
std::atomic<QueryVideoMemoryInfoFn> g_originalQuery{nullptr};
std::atomic<bool> g_firstQueryLogged{false};
std::atomic<bool> g_firstOverrideLogged{false};
std::atomic<bool> g_expiryLogged{false};
std::atomic<ULONGLONG> g_overrideStartedAt{kOverrideNotStarted};
std::atomic<bool> g_startRequested{false};
ULONGLONG g_initializeTick = 0;

void logMessage(const char *format, ...) noexcept
{
	AcquireSRWLockExclusive(&g_logLock);
	if(g_log == nullptr) {
		ReleaseSRWLockExclusive(&g_logLock);
		return;
	}

	SYSTEMTIME time{};
	GetLocalTime(&time);
	std::fprintf(
	    g_log,
	    "[%02u:%02u:%02u.%03u] ",
	    time.wHour,
	    time.wMinute,
	    time.wSecond,
	    time.wMilliseconds);
	va_list args;
	va_start(args, format);
	std::vfprintf(g_log, format, args);
	va_end(args);
	std::fputc('\n', g_log);
	std::fflush(g_log);
	ReleaseSRWLockExclusive(&g_logLock);
}

void initializeLog() noexcept
{
	if(g_module == nullptr) {
		return;
	}

	constexpr DWORD kModulePathLength = 32768;
	wchar_t modulePath[kModulePathLength]{};
	const DWORD length = GetModuleFileNameW(g_module, modulePath, kModulePathLength);
	if(length == 0 || length >= kModulePathLength) {
		return;
	}

	size_t logPathOffset = length;
	while(logPathOffset > 0 && modulePath[logPathOffset - 1] != L'\\' && modulePath[logPathOffset - 1] != L'/') {
		--logPathOffset;
	}
	if(logPathOffset == 0) {
		return;
	}
	constexpr wchar_t kLogFileName[] = L"UE5LumenFix.log";
	if(logPathOffset + _countof(kLogFileName) > kModulePathLength) {
		return;
	}
	std::memcpy(modulePath + logPathOffset, kLogFileName, sizeof(kLogFileName));

	AcquireSRWLockExclusive(&g_logLock);
	if(g_log == nullptr) {
		std::FILE *file = nullptr;
		if(_wfopen_s(&file, modulePath, L"w") == 0) {
			g_log = file;
		}
	}
	ReleaseSRWLockExclusive(&g_logLock);
}

HRESULT installAdapterHook(IDXGIAdapter3 *adapter)
{
	auto **vtable = *reinterpret_cast<void ***>(adapter);
	auto *slot = &vtable[kQueryVideoMemoryInfoVtableIndex];
	const auto hookAddress = reinterpret_cast<void *>(&hookQueryVideoMemoryInfo);
	void *current = *slot;

	if(current == hookAddress) {
		return g_originalQuery.load(std::memory_order_acquire) != nullptr ? S_OK : E_UNEXPECTED;
	}
	if(current == nullptr) {
		return E_POINTER;
	}

	g_originalQuery.store(reinterpret_cast<QueryVideoMemoryInfoFn>(current), std::memory_order_release);

	DWORD oldProtection = 0;
	if(!VirtualProtect(slot, sizeof(void *), PAGE_EXECUTE_READWRITE, &oldProtection)) {
		DWORD error = GetLastError();
		if(error == ERROR_SUCCESS) {
			error = ERROR_ACCESS_DENIED;
		}
		g_originalQuery.store(nullptr, std::memory_order_release);
		return HRESULT_FROM_WIN32(error);
	}

	const auto previous = InterlockedCompareExchangePointer(
	    reinterpret_cast<void *volatile *>(slot),
	    hookAddress,
	    current);
	DWORD ignoredProtection = 0;
	const BOOL protectionRestored = VirtualProtect(slot, sizeof(void *), oldProtection, &ignoredProtection);

	if(previous != current) {
		if(previous == hookAddress) {
			return g_originalQuery.load(std::memory_order_acquire) != nullptr ? S_OK : E_UNEXPECTED;
		}
		g_originalQuery.store(nullptr, std::memory_order_release);
		return E_UNEXPECTED;
	}

	if(!protectionRestored) {
		logMessage("hook installed; failed to restore vtable protection (Win32 error %lu)", GetLastError());
	}
	return S_OK;
}

HRESULT installHook()
{
	IDXGIFactory1 *factory = nullptr;
	HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
	if(FAILED(result)) {
		return result;
	}

	HRESULT lastError = DXGI_ERROR_NOT_FOUND;
	for(UINT index = 0;; ++index) {
		IDXGIAdapter1 *adapter = nullptr;
		result = factory->EnumAdapters1(index, &adapter);
		if(result == DXGI_ERROR_NOT_FOUND) {
			break;
		}
		if(FAILED(result)) {
			lastError = result;
			break;
		}

		IDXGIAdapter3 *adapter3 = nullptr;
		result = adapter->QueryInterface(IID_PPV_ARGS(&adapter3));
		adapter->Release();
		if(FAILED(result)) {
			lastError = result;
			continue;
		}

		result = installAdapterHook(adapter3);
		adapter3->Release();
		if(SUCCEEDED(result)) {
			factory->Release();
			return S_OK;
		}
		lastError = result;
	}

	factory->Release();
	return lastError;
}

HRESULT STDMETHODCALLTYPE hookQueryVideoMemoryInfo(
    IDXGIAdapter3 *adapter,
    UINT nodeIndex,
    DXGI_MEMORY_SEGMENT_GROUP group,
    DXGI_QUERY_VIDEO_MEMORY_INFO *info)
{
	const auto original = g_originalQuery.load(std::memory_order_acquire);
	if(original == nullptr) {
		return E_UNEXPECTED;
	}

	const HRESULT result = original(adapter, nodeIndex, group, info);
	const bool hasOriginalBudget = SUCCEEDED(result) && info != nullptr;
	const UINT64 originalBudget = hasOriginalBudget ? info->Budget : 0;

	if(!g_firstQueryLogged.exchange(true, std::memory_order_relaxed)) {
		if(hasOriginalBudget) {
			logMessage(
			    "first QueryVideoMemoryInfo call: NodeIndex=%u group=%u original Budget=%llu HRESULT=0x%08lX",
			    nodeIndex,
			    static_cast<unsigned>(group),
			    static_cast<unsigned long long>(originalBudget),
			    static_cast<unsigned long>(result));
		} else {
			logMessage(
			    "first QueryVideoMemoryInfo call: NodeIndex=%u group=%u original Budget=unavailable HRESULT=0x%08lX",
			    nodeIndex,
			    static_cast<unsigned>(group),
			    static_cast<unsigned long>(result));
		}
	}

	if(!hasOriginalBudget || nodeIndex != 0 || group != DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL || originalBudget != 0) {
		return result;
	}

	const ULONGLONG now = GetTickCount64();
	ULONGLONG startedAt = g_overrideStartedAt.load(std::memory_order_relaxed);
	if(startedAt == kOverrideNotStarted) {
		ULONGLONG expected = kOverrideNotStarted;
		if(g_overrideStartedAt.compare_exchange_strong(expected, now, std::memory_order_relaxed)) {
			startedAt = now;
		} else {
			startedAt = expected;
		}
	}

	if(now - startedAt >= kOverrideDurationMs) {
		if(!g_expiryLogged.exchange(true, std::memory_order_relaxed)) {
			logMessage("temporary 60-second Budget override expired");
		}
		return result;
	}

	info->Budget = kNonLocalBudget;
	if(!g_firstOverrideLogged.exchange(true, std::memory_order_relaxed)) {
		logMessage(
		    "first Budget override: NodeIndex=%u group=%u 0 -> %llu",
		    nodeIndex,
		    static_cast<unsigned>(group),
		    static_cast<unsigned long long>(kNonLocalBudget));
	}
	return result;
}

DWORD WINAPI initializeWorker(LPVOID)
{
	const ULONGLONG workerTick = GetTickCount64();
	initializeLog();
	logMessage(
	    "worker started: delay=%llums",
	    static_cast<unsigned long long>(workerTick - g_initializeTick));
	logMessage("InitializeASI entered");
	logMessage("UE5LumenFix ASI version %s; process id=%lu", kVersion, GetCurrentProcessId());

	const HRESULT result = installHook();
	if(SUCCEEDED(result)) {
		logMessage("hook installed");
	} else {
		logMessage("hook installation failed: HRESULT=0x%08lX", static_cast<unsigned long>(result));
	}
	return 0;
}

}  // namespace

extern "C" BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID)
{
	if(reason == DLL_PROCESS_ATTACH) {
		g_module = module;
	}
	return TRUE;
}

extern "C" __declspec(dllexport) void InitializeASI()
{
	const ULONGLONG entryTick = GetTickCount64();
	bool expected = false;
	if(!g_startRequested.compare_exchange_strong(expected, true)) {
		return;
	}

	g_initializeTick = entryTick;
	HANDLE worker = CreateThread(nullptr, 0, initializeWorker, nullptr, 0, nullptr);
	if(worker == nullptr) {
		OutputDebugStringA("[UE5LumenFix] Failed to start hook worker\n");
		return;
	}
	CloseHandle(worker);
}
