/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
// win_splitproc.c -- Windows shim for splitscreen Independent mode (design 17.2,
// code/client/cl_splitindep.c): launch flags, child processes (in a kill-on-close
// job, so no child outlives the coordinator), a loopback UDP socket for the
// coordinator <-> child messages, and the game window's placement.  Other
// platforms get failing stubs in cl_splitindep.c.

#include <winsock2.h>
#include <ws2tcpip.h>
#include "../client/client.h"
#include "win_local.h"
#include <shobjidl.h>	// ITaskbarList2 (R16: tiles marked fullscreen for the shell)

#define SPLIT_MAX_PROCS	16

static int		splitFlags;
static HANDLE	splitJob;
static struct {
	qboolean	used;
	HANDLE		process;
	DWORD		pid;
} splitProcs[SPLIT_MAX_PROCS];

static SOCKET	splitSock = INVALID_SOCKET;
static int		splitSockPort;
static qboolean	splitWsa;


/*
==================
Sys_SplitParseFlags

Launch flags before the first '+' of the command line (WinMain, before the
console window exists): --independent, --child, --noactivate.
==================
*/
void Sys_SplitParseFlags( const char *cmdline ) {
	char word[64];
	const char *p = cmdline;
	int len;

	splitFlags = 0;
	while ( p && *p && *p != '+' ) {
		while ( *p == ' ' || *p == '\t' ) {
			p++;
		}
		for ( len = 0; p[len] && p[len] != ' ' && p[len] != '\t' && p[len] != '+'; len++ )
			;
		if ( !len ) {
			break;
		}
		Q_strncpyz( word, p, MIN( len + 1, (int)sizeof( word ) ) );
		if ( !Q_stricmp( word, "--independent" ) ) {
			splitFlags |= SPLIT_FLAG_INDEPENDENT;
		} else if ( !Q_stricmp( word, "--child" ) ) {
			splitFlags |= SPLIT_FLAG_CHILD | SPLIT_FLAG_NOACTIVATE;
		} else if ( !Q_stricmp( word, "--noactivate" ) ) {
			splitFlags |= SPLIT_FLAG_NOACTIVATE | SPLIT_FLAG_BOTTOM;
		}
		p += len;
	}
}


int Sys_SplitLaunchFlags( void ) {
	return splitFlags;
}


// the early console window (win_syscon.c) stays hidden instead of taking the foreground
qboolean Sys_SplitNoActivate( void ) {
	return ( splitFlags & SPLIT_FLAG_NOACTIVATE ) ? qtrue : qfalse;
}


int Sys_SplitPid( void ) {
	return (int)GetCurrentProcessId();
}


/*
==================
Sys_SplitSpawn

Start this executable again with 'args' (everything after the exe path).
The new process never takes the foreground: its windows show without
activation (--child / --noactivate, see GLW_ShowWindow) and it starts with
SW_SHOWNOACTIVATE as its default show command.  Returns a handle or -1.
==================
*/
int Sys_SplitSpawn( const char *args ) {
	static WCHAR cmd[MAX_CMDLINE_CHARS + 2048];
	WCHAR exe[2048];
	STARTUPINFOW si;
	PROCESS_INFORMATION pi;
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION jl;
	DWORD exeLen;
	int h, len;

	for ( h = 0; h < SPLIT_MAX_PROCS && splitProcs[h].used; h++ )
		;
	if ( h == SPLIT_MAX_PROCS ) {
		return -1;
	}
	// wide: the install path may hold characters outside the ANSI code page
	exeLen = GetModuleFileNameW( NULL, exe, (DWORD)ARRAYSIZE( exe ) );
	if ( !exeLen || exeLen >= ARRAYSIZE( exe ) ) {
		Com_Printf( S_COLOR_YELLOW "indep: GetModuleFileName failed (error %lu)\n", GetLastError() );
		return -1;
	}
	// "exe" args -- the arguments are engine strings (ANSI code page, like every engine path)
	len = (int)exeLen + 3;
	if ( len >= (int)ARRAYSIZE( cmd ) ) {
		return -1;
	}
	cmd[0] = L'"';
	Com_Memcpy( cmd + 1, exe, exeLen * sizeof( WCHAR ) );
	cmd[exeLen + 1] = L'"';
	cmd[exeLen + 2] = L' ';
	if ( !MultiByteToWideChar( CP_ACP, 0, args, -1, cmd + len, (int)ARRAYSIZE( cmd ) - len ) ) {
		Com_Printf( S_COLOR_YELLOW "indep: child command line does not convert (error %lu)\n", GetLastError() );
		return -1;
	}

	// every child lives in one job that is killed when this process ends, however it ends
	if ( !splitJob ) {
		splitJob = CreateJobObjectA( NULL, NULL );
		if ( splitJob ) {
			Com_Memset( &jl, 0, sizeof( jl ) );
			jl.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
			if ( !SetInformationJobObject( splitJob, JobObjectExtendedLimitInformation, &jl, sizeof( jl ) ) ) {
				Com_Printf( S_COLOR_YELLOW "indep: no kill-on-close job (error %lu); children quit on their own when this window goes\n",
					GetLastError() );
				CloseHandle( splitJob );
				splitJob = NULL;
			}
		}
	}

	Com_Memset( &si, 0, sizeof( si ) );
	si.cb = sizeof( si );
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_SHOWNOACTIVATE;
	Com_Memset( &pi, 0, sizeof( pi ) );

	if ( !CreateProcessW( exe, cmd, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi ) ) {
		Com_Printf( S_COLOR_YELLOW "indep: CreateProcess failed (error %lu)\n", GetLastError() );
		return -1;
	}
	if ( splitJob && !AssignProcessToJobObject( splitJob, pi.hProcess ) ) {
		Com_Printf( S_COLOR_YELLOW "indep: child not in the kill-on-exit job (error %lu); it quits on its own when this window goes\n",
			GetLastError() );
	}
	ResumeThread( pi.hThread );
	CloseHandle( pi.hThread );

	splitProcs[h].used = qtrue;
	splitProcs[h].process = pi.hProcess;
	splitProcs[h].pid = pi.dwProcessId;
	return h;
}


int Sys_SplitProcPid( int h ) {
	return ( h >= 0 && h < SPLIT_MAX_PROCS && splitProcs[h].used ) ? (int)splitProcs[h].pid : 0;
}


qboolean Sys_SplitProcRunning( int h ) {
	if ( h < 0 || h >= SPLIT_MAX_PROCS || !splitProcs[h].used ) {
		return qfalse;
	}
	return ( WaitForSingleObject( splitProcs[h].process, 0 ) == WAIT_TIMEOUT ) ? qtrue : qfalse;
}


void Sys_SplitProcKill( int h ) {
	if ( h >= 0 && h < SPLIT_MAX_PROCS && splitProcs[h].used ) {
		TerminateProcess( splitProcs[h].process, 1 );
		WaitForSingleObject( splitProcs[h].process, 2000 );
	}
}


void Sys_SplitProcClose( int h ) {
	if ( h >= 0 && h < SPLIT_MAX_PROCS && splitProcs[h].used ) {
		CloseHandle( splitProcs[h].process );
		Com_Memset( &splitProcs[h], 0, sizeof( splitProcs[h] ) );
	}
}


// another process (the coordinator, seen from a child) still runs
qboolean Sys_SplitPidRunning( int pid ) {
	HANDLE p;
	DWORD r;

	if ( pid <= 0 ) {
		return qfalse;
	}
	p = OpenProcess( SYNCHRONIZE, FALSE, (DWORD)pid );
	if ( !p ) {
		return ( GetLastError() == ERROR_ACCESS_DENIED ) ? qtrue : qfalse;
	}
	r = WaitForSingleObject( p, 0 );
	CloseHandle( p );
	return ( r == WAIT_TIMEOUT ) ? qtrue : qfalse;
}


// let a child take the foreground once (IPC 'focus')
void Sys_SplitAllowFocus( int pid ) {
	AllowSetForegroundWindow( (DWORD)pid );
}


void Sys_SplitFocus( void ) {
	if ( g_wv.hWnd ) {
		SetForegroundWindow( g_wv.hWnd );
	}
}


/*
==================
Loopback UDP: one non-blocking socket per process on 127.0.0.1, ephemeral port
==================
*/
int Sys_SplitSockOpen( void ) {
	struct sockaddr_in a;
	int len = sizeof( a );
	u_long nb = 1;
	WSADATA wsa;

	if ( splitSock != INVALID_SOCKET ) {
		return splitSockPort;
	}
	if ( !splitWsa ) {
		if ( WSAStartup( MAKEWORD( 2, 2 ), &wsa ) != 0 ) {
			return 0;
		}
		splitWsa = qtrue;
	}
	splitSock = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
	if ( splitSock == INVALID_SOCKET ) {
		return 0;
	}
	Com_Memset( &a, 0, sizeof( a ) );
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
	a.sin_port = 0;
	if ( bind( splitSock, (struct sockaddr *)&a, sizeof( a ) ) == SOCKET_ERROR
		|| ioctlsocket( splitSock, FIONBIO, &nb ) == SOCKET_ERROR
		|| getsockname( splitSock, (struct sockaddr *)&a, &len ) == SOCKET_ERROR ) {
		closesocket( splitSock );
		splitSock = INVALID_SOCKET;
		return 0;
	}
	splitSockPort = ntohs( a.sin_port );
	return splitSockPort;
}


void Sys_SplitSockClose( void ) {
	if ( splitSock != INVALID_SOCKET ) {
		closesocket( splitSock );
		splitSock = INVALID_SOCKET;
	}
	splitSockPort = 0;
	if ( splitWsa ) {
		WSACleanup();
		splitWsa = qfalse;
	}
}


void Sys_SplitSockSend( int port, const char *text ) {
	struct sockaddr_in a;

	if ( splitSock == INVALID_SOCKET || port <= 0 ) {
		return;
	}
	Com_Memset( &a, 0, sizeof( a ) );
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
	a.sin_port = htons( (u_short)port );
	sendto( splitSock, text, (int)strlen( text ), 0, (struct sockaddr *)&a, sizeof( a ) );
}


// one datagram from 127.0.0.1 into buf (0-terminated); its length, or -1 = none waiting
int Sys_SplitSockRecv( char *buf, int size, int *fromPort ) {
	struct sockaddr_in a;
	int len = sizeof( a ), n;

	while ( splitSock != INVALID_SOCKET ) {
		n = recvfrom( splitSock, buf, size - 1, 0, (struct sockaddr *)&a, &len );
		if ( n == SOCKET_ERROR ) {
			if ( WSAGetLastError() == WSAECONNRESET || WSAGetLastError() == WSAEMSGSIZE ) {
				len = sizeof( a );
				continue;	// an ICMP "port unreachable" of an earlier send (a child that is gone), or junk
			}
			return -1;
		}
		if ( a.sin_addr.s_addr != htonl( INADDR_LOOPBACK ) ) {
			len = sizeof( a );
			continue;
		}
		buf[n] = '\0';
		*fromPort = ntohs( a.sin_port );
		return n;
	}
	return -1;
}


/*
==================
Game window placement
==================
*/
qboolean Sys_SplitWindowGet( int *x, int *y, int *w, int *h ) {
	RECT r;

	if ( !g_wv.hWnd || !GetWindowRect( g_wv.hWnd, &r ) ) {
		return qfalse;
	}
	*x = r.left;
	*y = r.top;
	*w = r.right - r.left;
	*h = r.bottom - r.top;
	return qtrue;
}


// a re-tile recreates the window (vid_restart): it takes the foreground again only if it has it now
static qboolean	splitReshow;
static qboolean	splitReshowActive;

void Sys_SplitKeepFocus( void ) {
	splitReshow = qtrue;
	splitReshowActive = ( g_wv.hWnd && GetForegroundWindow() == g_wv.hWnd ) ? qtrue : qfalse;
}


// the whole monitor the game window is on (before it exists: the one at vid_xpos/vid_ypos)
qboolean Sys_SplitMonitorArea( int *x, int *y, int *w, int *h ) {
	HMONITOR mon;
	MONITORINFO mi;
	POINT pt;

	if ( g_wv.hWnd ) {
		mon = MonitorFromWindow( g_wv.hWnd, MONITOR_DEFAULTTOPRIMARY );
	} else {
		pt.x = Cvar_VariableIntegerValue( "vid_xpos" );
		pt.y = Cvar_VariableIntegerValue( "vid_ypos" );
		mon = MonitorFromPoint( pt, MONITOR_DEFAULTTOPRIMARY );
	}
	Com_Memset( &mi, 0, sizeof( mi ) );
	mi.cbSize = sizeof( mi );
	if ( !mon || !GetMonitorInfo( mon, &mi ) ) {
		return qfalse;
	}
	*x = mi.rcMonitor.left;
	*y = mi.rcMonitor.top;
	*w = mi.rcMonitor.right - mi.rcMonitor.left;
	*h = mi.rcMonitor.bottom - mi.rcMonitor.top;
	return qtrue;
}


/*
==================
GLW_SplitShowWindow

The game window's show after the renderer started (win_glimp.c).  --child /
--noactivate: shown without taking the foreground (and, for --noactivate
test runs, below every other window); its WM_ACTIVATE never comes, so it
starts inactive (no mouse grab).  A window recreated for a re-tile takes the
foreground only if the old one had it.  An Independent-mode window that is
still visible (vid_restart fast) is not shown again: SW_SHOW would activate it.
==================
*/
void GLW_SplitShowWindow( void ) {
	qboolean reshow = splitReshow, reshowActive = splitReshowActive;

	splitReshow = qfalse;
	if ( !g_wv.hWnd ) {
		return;
	}
	if ( IsWindowVisible( g_wv.hWnd ) && CL_SplitWindowRect( NULL, NULL, NULL, NULL ) ) {
		GLW_SplitMarkShell();
		return;
	}
	if ( ( splitFlags & SPLIT_FLAG_NOACTIVATE ) || ( reshow && !reshowActive ) ) {
		ShowWindow( g_wv.hWnd, SW_SHOWNOACTIVATE );
		if ( splitFlags & SPLIT_FLAG_BOTTOM ) {
			SetWindowPos( g_wv.hWnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE );
		}
		if ( GetForegroundWindow() != g_wv.hWnd ) {
			gw_active = qfalse;
		}
		GLW_SplitMarkShell();
		return;
	}
	ShowWindow( g_wv.hWnd, SW_SHOW );
	GLW_SplitMarkShell();
}


/*
=============================================================================

R16: Independent-mode tiles marked fullscreen for the shell

A borderless tile is not monitor-sized, so the shell does not treat it as a
fullscreen app: the taskbar stays on top and SHQueryUserNotificationState
(what tools like Joyxoff read) says "desktop".  ITaskbarList2::
MarkFullscreenWindow tells the shell this window is fullscreen: while it is
the foreground window the taskbar goes behind it and the notification state
reports a fullscreen app.  cl_splitIndepMarkFullscreen 0 turns it off (next
vid_restart).  Never activates or moves the window.

=============================================================================
*/

// {56FDF344-FD6D-11d0-958A-006097C9A090}, {602D4995-B13A-429b-A66E-1935E44F4317}
static const GUID splitCLSID_TaskbarList = { 0x56fdf344, 0xfd6d, 0x11d0, { 0x95, 0x8a, 0x00, 0x60, 0x97, 0xc9, 0xa0, 0x90 } };
static const GUID splitIID_ITaskbarList2 = { 0x602d4995, 0xb13a, 0x429b, { 0xa6, 0x6e, 0x19, 0x35, 0xe4, 0x4f, 0x43, 0x17 } };

static ITaskbarList2	*splitTaskbar;
static qboolean			splitTaskbarFailed;
static HWND				splitMarkedHwnd;

static int Split_PlayerNum( void ) {
	const int child = Cvar_VariableIntegerValue( "cl_splitChild" );
	return child > 1 ? child : 1;
}


static qboolean Split_MarkFullscreen( HWND hwnd, BOOL on ) {
	HRESULT hr;

	if ( !splitTaskbar ) {
		if ( splitTaskbarFailed ) {
			return qfalse;
		}
		// S_OK / S_FALSE (already, e.g. by SDL or the sound code) / RPC_E_CHANGED_MODE
		// (an MTA thread): COM is usable in every case; kept for the process' life
		CoInitializeEx( NULL, COINIT_APARTMENTTHREADED );
		hr = CoCreateInstance( &splitCLSID_TaskbarList, NULL, CLSCTX_INPROC_SERVER, &splitIID_ITaskbarList2, (void **)&splitTaskbar );
		if ( SUCCEEDED( hr ) && splitTaskbar ) {
			hr = splitTaskbar->lpVtbl->HrInit( splitTaskbar );
			if ( FAILED( hr ) ) {
				splitTaskbar->lpVtbl->Release( splitTaskbar );
			}
		}
		if ( FAILED( hr ) || !splitTaskbar ) {
			splitTaskbar = NULL;
			splitTaskbarFailed = qtrue;
			Com_Printf( S_COLOR_YELLOW "window: P%i taskbar list unavailable (HRESULT 0x%08lx): not marked fullscreen for the shell\n",
				Split_PlayerNum(), (unsigned long)hr );
			return qfalse;
		}
	}
	hr = splitTaskbar->lpVtbl->MarkFullscreenWindow( splitTaskbar, hwnd, on );
	if ( FAILED( hr ) ) {
		Com_Printf( S_COLOR_YELLOW "window: P%i MarkFullscreenWindow(%s) failed (HRESULT 0x%08lx)\n", Split_PlayerNum(), on ? "TRUE" : "FALSE", (unsigned long)hr );
		return qfalse;
	}
	Com_Printf( on ? "window: P%i marked fullscreen for the shell\n" : "window: P%i no longer marked fullscreen for the shell\n", Split_PlayerNum() );
	return qtrue;
}


/*
==================
GLW_SplitMarkShell

After the game window is shown (GLW_SplitShowWindow): an Independent-mode tile
is marked, a Together window (the mode switch recreates the window) is not.
==================
*/
void GLW_SplitMarkShell( void ) {
	static cvar_t *mark;
	qboolean want;

	if ( !mark ) {
		mark = Cvar_Get( "cl_splitIndepMarkFullscreen", "1", CVAR_ARCHIVE_ND );
		Cvar_CheckRange( mark, "0", "1", CV_INTEGER );
		Cvar_SetDescription( mark, "Independent mode (Windows): mark every borderless tile window fullscreen for the shell, so the taskbar "
			"hides behind the active tile and fullscreen-detection tools see a game. Applies at the next vid_restart." );
	}
	want = ( g_wv.hWnd && mark->integer && CL_SplitWindowRect( NULL, NULL, NULL, NULL ) ) ? qtrue : qfalse;
	if ( splitMarkedHwnd && ( !want || splitMarkedHwnd != g_wv.hWnd ) ) {
		if ( IsWindow( splitMarkedHwnd ) ) {
			Split_MarkFullscreen( splitMarkedHwnd, FALSE );
		}
		splitMarkedHwnd = NULL;
	}
	if ( want && splitMarkedHwnd != g_wv.hWnd && Split_MarkFullscreen( g_wv.hWnd, TRUE ) ) {
		splitMarkedHwnd = g_wv.hWnd;
	}
}


// before the game window is destroyed (vid_restart, the Together switch, quit)
void GLW_SplitUnmarkShell( HWND hwnd ) {
	if ( hwnd && hwnd == splitMarkedHwnd ) {
		Split_MarkFullscreen( hwnd, FALSE );
		splitMarkedHwnd = NULL;
	}
}

