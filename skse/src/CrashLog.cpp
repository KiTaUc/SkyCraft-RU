#include "Game.h"

#include <DbgHelp.h>

#pragma comment(lib, "dbghelp.lib")

// When Skyrim crashes: where (module + offset, and the call stack) into SkyCraft.log, and a small
// minidump (SkyCraft_crash.dmp next to it) to look at. Chains to whatever handler came before.
namespace skycraft::CrashLog
{
	namespace
	{
		LPTOP_LEVEL_EXCEPTION_FILTER previous = nullptr;
		std::atomic<bool>            handled{ false };

		struct Frame
		{
			DWORD64 address;
		};

		// Walks the stack from the crash context (no C++ objects here: it runs under __try).
		int Walk(CONTEXT a_context, Frame* a_out, int a_max)
		{
			int n = 0;
			__try {
				for (; n < a_max; ++n) {
					a_out[n].address = a_context.Rip;
					DWORD64 imageBase = 0;
					auto*   function = RtlLookupFunctionEntry(a_context.Rip, &imageBase, nullptr);
					if (!function) {
						a_context.Rip = *reinterpret_cast<DWORD64*>(a_context.Rsp);
						a_context.Rsp += 8;
					} else {
						PVOID   handlerData = nullptr;
						DWORD64 establisher = 0;
						RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, a_context.Rip, function, &a_context, &handlerData, &establisher, nullptr);
					}
					if (!a_context.Rip) {
						++n;
						break;
					}
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
			}
			return n;
		}

		std::string Where(DWORD64 a_address)
		{
			HMODULE module = nullptr;
			if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(a_address), &module) || !module) {
				return std::format("{:#x}", a_address);
			}
			char path[MAX_PATH]{};
			GetModuleFileNameA(module, path, MAX_PATH);
			const char* name = std::strrchr(path, '\\');
			return std::format("{}+{:#x}", name ? name + 1 : path, a_address - reinterpret_cast<DWORD64>(module));
		}

		LONG WINAPI Filter(EXCEPTION_POINTERS* a_info)
		{
			if (!handled.exchange(true) && a_info && a_info->ExceptionRecord && a_info->ContextRecord) {
				const auto* record = a_info->ExceptionRecord;
				logger::critical("CRASH: exception {:#x} at {} (thread {})", record->ExceptionCode, Where(reinterpret_cast<DWORD64>(record->ExceptionAddress)), GetCurrentThreadId());
				if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
					logger::critical("CRASH: {} address {:#x}", record->ExceptionInformation[0] ? "writing" : "reading", record->ExceptionInformation[1]);
				}
				Frame frames[48];
				const int count = Walk(*a_info->ContextRecord, frames, 48);
				for (int i = 0; i < count; ++i) {
					logger::critical("CRASH:   {:2} {}", i, Where(frames[i].address));
				}
				if (auto dir = SKSE::log::log_directory()) {
					const auto file = *dir / "SkyCraft_crash.dmp";
					HANDLE     handle = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
					if (handle != INVALID_HANDLE_VALUE) {
						MINIDUMP_EXCEPTION_INFORMATION info{ GetCurrentThreadId(), a_info, FALSE };
						MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), handle,
							MINIDUMP_TYPE(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules), &info, nullptr, nullptr);
						CloseHandle(handle);
						logger::critical("CRASH: minidump written to {}", file.string());
					}
				}
				spdlog::default_logger()->flush();
			}
			return previous ? previous(a_info) : EXCEPTION_CONTINUE_SEARCH;
		}
	}

	void Install()
	{
		// Called at load and again once the game is up (in case something replaced it meanwhile).
		const auto old = SetUnhandledExceptionFilter(Filter);
		if (old != Filter) {
			previous = old;
		}
	}
}
