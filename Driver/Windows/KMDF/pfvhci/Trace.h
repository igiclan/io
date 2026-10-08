/*
 * ParsFlash USB Redirector - pfvhci.sys
 * https://parsflash.com
 *
 * Trace.h - minimal, always-buildable logging on top of DbgPrintEx.
 *
 * WPP/TraceLogging were deliberately not used: they need the WPP preprocessor
 * (tracewpp) or TraceLoggingProvider registration, which adds build steps that
 * could not be validated without a WDK. DbgPrintEx output is visible in WinDbg
 * (kernel debugger) and in Sysinternals DebugView ("Capture Kernel").
 *
 * Messages are filtered by the kernel per component and level. To see them:
 *   reg add "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Debug Print Filter" ^
 *       /v IHVDRIVER /t REG_DWORD /d 0xF
 * or in WinDbg:  ed nt!Kd_IHVDRIVER_Mask 0xF
 *
 * Levels (dpfilter.h): ERROR=0, WARNING=1, TRACE=2, INFO=3.
 *
 * All format strings must end with "\n". Do not use %wZ / %ws at IRQL > PASSIVE_LEVEL
 * (Unicode conversion may touch paged tables).
 */
#pragma once

#define PF_LOG(level, ...) DbgPrintEx(DPFLTR_IHVDRIVER_ID, (level), "pfvhci: " __VA_ARGS__)

#define PF_ERROR(...) PF_LOG(DPFLTR_ERROR_LEVEL, __VA_ARGS__)
#define PF_WARN(...)  PF_LOG(DPFLTR_WARNING_LEVEL, __VA_ARGS__)
#define PF_TRACE(...) PF_LOG(DPFLTR_TRACE_LEVEL, __VA_ARGS__)
#define PF_INFO(...)  PF_LOG(DPFLTR_INFO_LEVEL, __VA_ARGS__)

/*
 * Per-URB tracing is far too chatty for release builds (thousands of URBs per
 * second on a bulk pipe); it is compiled in only for checked/debug builds.
 */
#if DBG
#define PF_URB_TRACE(...) PF_LOG(DPFLTR_INFO_LEVEL, __VA_ARGS__)
#else
#define PF_URB_TRACE(...) ((void)0)
#endif
