/*
 * Win32 kernel functions
 *
 * Copyright 1995 Martin von Loewis
 */

#include <string.h>
#include <unistd.h>
#include <sys/times.h>
#include "windows.h"
#include "winerror.h"
#include "heap.h"
#include "thread.h"
#include "process.h"
#include "pe_image.h"
#include "file.h"
#include "task.h"
#include "toolhelp.h"
#include "debug.h"


/**********************************************************************
 *          GetProcessAffinityMask
 */
BOOL32 WINAPI GetProcessAffinityMask(HANDLE32 hProcess,
                                     LPDWORD lpProcessAffinityMask,
                                     LPDWORD lpSystemAffinityMask)
{
	TRACE(task,"(%x,%lx,%lx)\n",
		hProcess,(lpProcessAffinityMask?*lpProcessAffinityMask:0),
		(lpSystemAffinityMask?*lpSystemAffinityMask:0));
	/* It is definitely important for a process to know on what processor
	   it is running :-) */
	if(lpProcessAffinityMask)
		*lpProcessAffinityMask=1;
	if(lpSystemAffinityMask)
		*lpSystemAffinityMask=1;
	return TRUE;
}

/**********************************************************************
 *           SetThreadAffinityMask
 * Works now like the Windows95 (no MP support) version
 */
BOOL32 WINAPI SetThreadAffinityMask(HANDLE32 hThread, DWORD dwThreadAffinityMask)
{
	THDB	*thdb = THREAD_GetPtr( hThread, THREAD_SET_INFORMATION, NULL );

	if (!thdb) 
		return FALSE;
	if (dwThreadAffinityMask!=1) {
		WARN(thread,"(%d,%ld): only 1 processor supported.\n",
                    (int)hThread,dwThreadAffinityMask);
		K32OBJ_DecCount((K32OBJ*)thdb);
		return FALSE;
	}
	K32OBJ_DecCount((K32OBJ*)thdb);
	return TRUE;
}

/**********************************************************************
 *  CreateProcess32A [KERNEL32.171]
 */
BOOL32 WINAPI CreateProcess32A(
	LPCSTR appname,LPSTR cmdline,LPSECURITY_ATTRIBUTES processattributes,
        LPSECURITY_ATTRIBUTES threadattributes,BOOL32 inherithandles,
	DWORD creationflags,LPVOID env,LPCSTR curdir,
	LPSTARTUPINFO32A startupinfo,LPPROCESS_INFORMATION processinfo
) {
	HINSTANCE16 hInst = 0;
	if (processinfo) memset(processinfo, '\0', sizeof(*processinfo));

	FIXME(win32,"(%s,%s,%p,%p,%d,%08lx,%p,%s,%p,%p): calling WinExec32\n",
		appname,cmdline,processattributes,threadattributes,
		inherithandles,creationflags,env,curdir,startupinfo,processinfo);

	hInst = WinExec32(cmdline,TRUE);

        return hInst >= 32;
#if 0
	/* make from lcc uses system as fallback if CreateProcess returns
	   FALSE, so return false */
	return FALSE;
#endif
}

/**********************************************************************
 *  CreateProcess32W [KERNEL32.172]
 */
BOOL32 WINAPI CreateProcess32W(
	LPCWSTR appname,LPWSTR cmdline,LPSECURITY_ATTRIBUTES processattributes,
        LPSECURITY_ATTRIBUTES threadattributes,BOOL32 inherithandles,
	DWORD creationflags,LPVOID env,LPCWSTR curdir,
	LPSTARTUPINFO32W startupinfo,LPPROCESS_INFORMATION processinfo)
{
    FIXME(win32,"(%p,%s,%p,%p,%d,%08lx,%p,%s,%p,%p): stub\n",
            appname,debugstr_w(cmdline),processattributes,threadattributes,
            inherithandles,creationflags,env,debugstr_w(curdir),startupinfo,
	    processinfo );
    /* make from lcc uses system as fallback if CreateProcess returns
       FALSE, so return false */
    return FALSE;
}

/**********************************************************************
 *  ContinueDebugEvent [KERNEL32.146]
 */
BOOL32 WINAPI ContinueDebugEvent(DWORD pid,DWORD tid,DWORD contstatus) {
    FIXME(win32,"(%ld,%ld,%ld): stub\n",pid,tid,contstatus);
	return TRUE;
}

/*********************************************************************
 *	GetProcessTimes				[KERNEL32.262]
 *
 * FIXME: implement this better ...
 */
BOOL32 WINAPI GetProcessTimes(
	HANDLE32 hprocess,LPFILETIME lpCreationTime,LPFILETIME lpExitTime,
	LPFILETIME lpKernelTime, LPFILETIME lpUserTime
) {
	struct tms tms;

	times(&tms);
	DOSFS_UnixTimeToFileTime(tms.tms_utime,lpUserTime,0);
	DOSFS_UnixTimeToFileTime(tms.tms_stime,lpKernelTime,0);
	return TRUE;
}

