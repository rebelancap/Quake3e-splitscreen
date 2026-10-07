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
// unix_splitproc.c -- Linux (SDL) shim for splitscreen Independent mode (design
// 17.2/17.3, code/client/cl_splitindep.c); the counterpart of win32/win_splitproc.c.
//
// Child processes: fork + exec of /proc/self/exe, kept in our process group,
// PR_SET_PDEATHSIG(SIGTERM) instead of a kill-on-close job, waitpid(WNOHANG)
// instead of process handles.  IPC: the same loopback UDP datagrams.
//
// Windows: Wayland cannot place a client's windows, so in Independent mode
// (coordinator and children) SDL runs on its X11 driver (XWayland on a Plasma /
// GNOME Wayland desktop).  Under Gamescope (Steam Deck game mode: one focused
// fullscreen surface) the mode is unavailable and the game stays Together.
// Windows that must not take the focus (--child, --noactivate, a re-tile of a
// window that did not have it) are created hidden, get _NET_WM_USER_TIME 0
// (EWMH: "do not focus on map", honoured by KWin) and are then shown; libX11 is
// dlopen'd (no link dependency; SDL loads it anyway on its X11 driver).
//
// Only Linux SDL builds link this file; the other non-Windows builds use the
// weak failing stubs in cl_splitindep.c.

#ifdef __linux__

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <dlfcn.h>

#include <SDL.h>
#include <SDL_syswm.h>

#include "../client/client.h"
#include "../sdl/sdl_glw.h"
#include "linux_local.h"

#define SPLIT_MAX_PROCS	16
#define SPLIT_MAX_ARGS	1024

static int		splitFlags;
static struct {
	qboolean	used;
	qboolean	reaped;		// waitpid collected it
	pid_t		pid;
} splitProcs[SPLIT_MAX_PROCS];

static int		splitSock = -1;
static int		splitSockPort;


/*
==================
Sys_SplitUnavailable

Why Independent mode cannot run here, NULL = it can.  Gamescope (Steam Deck
game mode) shows one focused fullscreen surface: separate windows cannot be
tiled there.  Without an X server (a Wayland session without XWayland) no
window can be placed.
==================
*/
const char *Sys_SplitUnavailable( void ) {
	const char *desk = getenv( "XDG_CURRENT_DESKTOP" );

	if ( getenv( "GAMESCOPE_WAYLAND_DISPLAY" ) || ( desk && !Q_stricmp( desk, "gamescope" ) ) ) {
		return "under Gamescope (Deck game mode)";	// (fits the host page row)
	}
	if ( !getenv( "DISPLAY" ) || !getenv( "DISPLAY" )[0] ) {
		return "without an X11 display (XWayland)";
	}
	return NULL;
}


// SDL's X11 driver from now on (an environment variable: SDL is not up yet, children inherit it)
static void Split_UseX11( void ) {
	setenv( "SDL_VIDEODRIVER", "x11", 1 );		// SDL2's name
	setenv( "SDL_VIDEO_DRIVER", "x11", 1 );		// SDL3's (sdl2-compat)
}


/*
==================
Sys_SplitParseFlags

Launch flags before the first '+' of the merged command line (main, before
Com_Init): --independent, --child, --noactivate.  Independent mode's windows
need the X11 video driver, set here before anything starts SDL.
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
	// (--noactivate test runs too: only X11 lets a window open without taking the focus)
	if ( ( splitFlags & ( SPLIT_FLAG_INDEPENDENT | SPLIT_FLAG_CHILD | SPLIT_FLAG_NOACTIVATE ) ) && !Sys_SplitUnavailable() ) {
		Split_UseX11();
	}
}


int Sys_SplitLaunchFlags( void ) {
	return splitFlags;
}


int Sys_SplitPid( void ) {
	return (int)getpid();
}


/*
==================
Split_SplitArgs

'args' -> argv words: split at spaces outside double quotes; the quotes stay
in the words, so the child's main() joins them back into the very line the
coordinator built (its Com_ParseCommandLine honours the quotes).
==================
*/
static int Split_SplitArgs( char *args, char **argv, int max ) {
	char *p = args;
	int n = 0, inq;

	while ( *p && n < max ) {
		while ( *p == ' ' ) {
			p++;
		}
		if ( !*p ) {
			break;
		}
		argv[n++] = p;
		for ( inq = 0; *p && ( inq || *p != ' ' ); p++ ) {
			if ( *p == '"' ) {
				inq = !inq;
			}
		}
		if ( *p ) {
			*p++ = '\0';
		}
	}
	while ( *p == ' ' ) {
		p++;
	}
	return *p ? -1 : n;	// -1: more than max words (the line would be cut)
}


/*
==================
Sys_SplitSpawn

Start this executable again with 'args' (everything after the exe path).
The child stays in our process group, gets SIGTERM when we die (PDEATHSIG;
it also watches our pid), stdin from /dev/null (our tty console keeps the
terminal) and none of our other file descriptors.  Returns a handle or -1.
==================
*/
int Sys_SplitSpawn( const char *args ) {
	static char line[MAX_CMDLINE_CHARS + 2048];
	static char exe[PATH_MAX];
	char *argv[SPLIT_MAX_ARGS + 2];
	struct rlimit rl;
	sigset_t none;
	pid_t pid, parent;
	int h, n, nullfd, maxfd, fd;
	ssize_t len;

	for ( h = 0; h < SPLIT_MAX_PROCS && splitProcs[h].used; h++ )
		;
	if ( h == SPLIT_MAX_PROCS ) {
		return -1;
	}
	len = readlink( "/proc/self/exe", exe, sizeof( exe ) - 1 );
	if ( len <= 0 ) {
		Com_Printf( S_COLOR_YELLOW "indep: /proc/self/exe unreadable (%s)\n", strerror( errno ) );
		return -1;
	}
	exe[len] = '\0';
	if ( strlen( args ) >= sizeof( line ) ) {
		return -1;
	}
	Q_strncpyz( line, args, sizeof( line ) );
	argv[0] = exe;
	n = Split_SplitArgs( line, argv + 1, SPLIT_MAX_ARGS );
	if ( n < 0 ) {
		Com_Printf( S_COLOR_YELLOW "indep: child command line has more than %i words\n", SPLIT_MAX_ARGS );
		return -1;
	}
	argv[n + 1] = NULL;

	// everything the child needs is prepared before fork: after it only async-signal-safe calls
	nullfd = open( "/dev/null", O_RDONLY | O_CLOEXEC );
	maxfd = ( getrlimit( RLIMIT_NOFILE, &rl ) == 0 && rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur < 65536 ) ? (int)rl.rlim_cur : 65536;
	sigemptyset( &none );
	parent = getpid();

	pid = fork();
	if ( pid < 0 ) {
		Com_Printf( S_COLOR_YELLOW "indep: fork failed (%s)\n", strerror( errno ) );
		if ( nullfd >= 0 ) {
			close( nullfd );
		}
		return -1;
	}
	if ( pid == 0 ) {
		// child: die with the coordinator (kill -9 too); it may already be gone
		prctl( PR_SET_PDEATHSIG, SIGTERM );
		if ( getppid() != parent ) {
			_exit( 1 );
		}
		if ( nullfd >= 0 ) {
			dup2( nullfd, 0 );
		}
		// no inherited sockets (game port, IPC), logs, pipes; the engine ignores SIGINT: undo
#ifdef SYS_close_range
		if ( syscall( SYS_close_range, 3U, ~0U, 0 ) != 0 )
#endif
		{
			for ( fd = 3; fd < maxfd; fd++ ) {
				close( fd );
			}
		}
		signal( SIGINT, SIG_DFL );
		signal( SIGHUP, SIG_DFL );
		signal( SIGPIPE, SIG_DFL );
		sigprocmask( SIG_SETMASK, &none, NULL );
		execv( exe, argv );
		_exit( 127 );
	}

	if ( nullfd >= 0 ) {
		close( nullfd );
	}
	splitProcs[h].used = qtrue;
	splitProcs[h].reaped = qfalse;
	splitProcs[h].pid = pid;
	return h;
}


int Sys_SplitProcPid( int h ) {
	return ( h >= 0 && h < SPLIT_MAX_PROCS && splitProcs[h].used ) ? (int)splitProcs[h].pid : 0;
}


// collect an exited child (no zombie); qtrue = it is gone
static qboolean Split_Reap( int h ) {
	int status;
	pid_t r;

	if ( splitProcs[h].reaped ) {
		return qtrue;
	}
	r = waitpid( splitProcs[h].pid, &status, WNOHANG );
	if ( r == splitProcs[h].pid || ( r < 0 && errno == ECHILD ) ) {
		splitProcs[h].reaped = qtrue;
		if ( r > 0 ) {
			if ( WIFSIGNALED( status ) ) {
				Com_DPrintf( "indep: pid %i ended by signal %i\n", (int)r, WTERMSIG( status ) );
			} else if ( WIFEXITED( status ) ) {
				Com_DPrintf( "indep: pid %i exited with code %i\n", (int)r, WEXITSTATUS( status ) );
			}
		}
		return qtrue;
	}
	return qfalse;
}


qboolean Sys_SplitProcRunning( int h ) {
	if ( h < 0 || h >= SPLIT_MAX_PROCS || !splitProcs[h].used ) {
		return qfalse;
	}
	return Split_Reap( h ) ? qfalse : qtrue;
}


// SIGTERM (the engine's handler quits cleanly), up to 2 s, then SIGKILL
void Sys_SplitProcKill( int h ) {
	int start;

	if ( h < 0 || h >= SPLIT_MAX_PROCS || !splitProcs[h].used || Split_Reap( h ) ) {
		return;
	}
	kill( splitProcs[h].pid, SIGTERM );
	for ( start = Sys_Milliseconds(); Sys_Milliseconds() - start < 2000; ) {
		if ( Split_Reap( h ) ) {
			return;
		}
		Sys_Sleep( 10 );
	}
	kill( splitProcs[h].pid, SIGKILL );
	for ( start = Sys_Milliseconds(); Sys_Milliseconds() - start < 2000 && !Split_Reap( h ); ) {
		Sys_Sleep( 10 );
	}
}


void Sys_SplitProcClose( int h ) {
	if ( h >= 0 && h < SPLIT_MAX_PROCS && splitProcs[h].used ) {
		Split_Reap( h );	// one still running is reaped by init once we are gone
		Com_Memset( &splitProcs[h], 0, sizeof( splitProcs[h] ) );
	}
}


// another process (the coordinator, seen from a child) still runs
qboolean Sys_SplitPidRunning( int pid ) {
	if ( pid <= 0 ) {
		return qfalse;
	}
	if ( kill( (pid_t)pid, 0 ) == 0 ) {
		// a child whose parent is gone has been re-parented (and got PDEATHSIG)
		return ( ( Sys_SplitLaunchFlags() & SPLIT_FLAG_CHILD ) && getppid() != (pid_t)pid ) ? qfalse : qtrue;
	}
	return ( errno == EPERM ) ? qtrue : qfalse;
}


// X11 has no AllowSetForegroundWindow: the window asks for the focus itself (Sys_SplitFocus)
void Sys_SplitAllowFocus( int pid ) {
}


void Sys_SplitFocus( void ) {
	if ( SDL_window ) {
		SDL_RaiseWindow( SDL_window );
	}
}


/*
==================
Loopback UDP: one non-blocking socket per process on 127.0.0.1, ephemeral port
==================
*/
int Sys_SplitSockOpen( void ) {
	struct sockaddr_in a;
	socklen_t len = sizeof( a );

	if ( splitSock >= 0 ) {
		return splitSockPort;
	}
	splitSock = socket( AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_UDP );
	if ( splitSock < 0 ) {
		return 0;
	}
	Com_Memset( &a, 0, sizeof( a ) );
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
	a.sin_port = 0;
	if ( bind( splitSock, (struct sockaddr *)&a, sizeof( a ) ) < 0
		|| getsockname( splitSock, (struct sockaddr *)&a, &len ) < 0 ) {
		close( splitSock );
		splitSock = -1;
		return 0;
	}
	splitSockPort = ntohs( a.sin_port );
	return splitSockPort;
}


void Sys_SplitSockClose( void ) {
	if ( splitSock >= 0 ) {
		close( splitSock );
		splitSock = -1;
	}
	splitSockPort = 0;
}


void Sys_SplitSockSend( int port, const char *text ) {
	struct sockaddr_in a;

	if ( splitSock < 0 || port <= 0 ) {
		return;
	}
	Com_Memset( &a, 0, sizeof( a ) );
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
	a.sin_port = htons( (unsigned short)port );
	sendto( splitSock, text, strlen( text ), MSG_NOSIGNAL, (struct sockaddr *)&a, sizeof( a ) );
}


// one datagram from 127.0.0.1 into buf (0-terminated); its length, or -1 = none waiting
int Sys_SplitSockRecv( char *buf, int size, int *fromPort ) {
	struct sockaddr_in a;
	socklen_t len;
	ssize_t n;

	while ( splitSock >= 0 ) {
		len = sizeof( a );
		n = recvfrom( splitSock, buf, size - 1, MSG_TRUNC, (struct sockaddr *)&a, &len );
		if ( n < 0 ) {
			if ( errno == ECONNREFUSED || errno == EINTR ) {
				continue;	// an ICMP "port unreachable" of an earlier send (a child that is gone)
			}
			return -1;		// EAGAIN: nothing waiting
		}
		if ( a.sin_addr.s_addr != htonl( INADDR_LOOPBACK ) || n > size - 1 ) {
			continue;	// not ours, or an oversized datagram (MSG_TRUNC: n is its real length) -- junk, as on Windows
		}
		buf[n] = '\0';
		*fromPort = ntohs( a.sin_port );
		return (int)n;
	}
	return -1;
}


/*
=============================================================================

GAME WINDOW (SDL; X11 for the no-focus show)

=============================================================================
*/

static qboolean	splitFocusLogged;
static qboolean	splitHadFocus;

qboolean Sys_SplitWindowGet( int *x, int *y, int *w, int *h ) {
	qboolean focus;

	if ( !SDL_window ) {
		return qfalse;
	}
	SDL_GetWindowPosition( SDL_window, x, y );
	SDL_GetWindowSize( SDL_window, w, h );
	// polled every 500 ms in Independent mode (Indep_LogRect): log this window's focus changes
	focus = ( SDL_GetWindowFlags( SDL_window ) & SDL_WINDOW_INPUT_FOCUS ) ? qtrue : qfalse;
	if ( !splitFocusLogged || focus != splitHadFocus ) {
		Com_Printf( "window: %s the input focus (SDL %s)\n", focus ? "has" : "does not have", SDL_GetCurrentVideoDriver() );
		splitFocusLogged = qtrue;
		splitHadFocus = focus;
	}
	return qtrue;
}


// a re-tile recreates the window (vid_restart): it takes the focus again only if it has it now
static qboolean	splitReshow;
static qboolean	splitReshowActive;

void Sys_SplitKeepFocus( void ) {
	splitReshow = qtrue;
	splitReshowActive = ( SDL_window && ( SDL_GetWindowFlags( SDL_window ) & SDL_WINDOW_INPUT_FOCUS ) ) ? qtrue : qfalse;
}


/*
==================
Sys_SplitMonitorArea

The usable area (without docked panels: _NET_WORKAREA) of the display the game
window is on; before it exists, of the display at vid_xpos/vid_ypos (SDL video
is started for the query and stopped again, on the X11 driver).
==================
*/
qboolean Sys_SplitMonitorArea( int *x, int *y, int *w, int *h ) {
	SDL_Rect r;
	qboolean started = qfalse, ok = qfalse;
	int d, n, px, py;

	if ( !SDL_WasInit( SDL_INIT_VIDEO ) ) {
		if ( !Sys_SplitUnavailable() ) {
			Split_UseX11();
		}
		if ( SDL_InitSubSystem( SDL_INIT_VIDEO ) != 0 ) {
			return qfalse;
		}
		started = qtrue;
	}
	d = SDL_window ? SDL_GetWindowDisplayIndex( SDL_window ) : -1;
	if ( d < 0 ) {
		px = Cvar_VariableIntegerValue( "vid_xpos" );
		py = Cvar_VariableIntegerValue( "vid_ypos" );
		n = SDL_GetNumVideoDisplays();
		for ( d = n - 1; d > 0; d-- ) {
			if ( SDL_GetDisplayBounds( d, &r ) == 0 && px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h ) {
				break;
			}
		}
	}
	if ( SDL_GetDisplayUsableBounds( d, &r ) == 0 || SDL_GetDisplayBounds( d, &r ) == 0 ) {
		*x = r.x;
		*y = r.y;
		*w = r.w;
		*h = r.h;
		ok = qtrue;
	}
	if ( started ) {
		SDL_QuitSubSystem( SDL_INIT_VIDEO );
	}
	return ok;
}


/*
==================
Sys_SplitNeedX11

sdl_glimp.c, before SDL video starts or a window is created: an Independent-
mode window needs SDL's X11 driver.  qtrue = SDL video runs on another driver
(the session mode was switched in place): the caller restarts SDL video (no
window exists then) and it comes back on X11.
==================
*/
qboolean Sys_SplitNeedX11( void ) {
	const char *drv;

	if ( !CL_SplitWindowRect( NULL, NULL, NULL, NULL ) || Sys_SplitUnavailable() ) {
		return qfalse;
	}
	drv = SDL_WasInit( SDL_INIT_VIDEO ) ? SDL_GetCurrentVideoDriver() : NULL;
	if ( drv && !Q_stricmp( drv, "x11" ) ) {
		return qfalse;
	}
	Split_UseX11();
	if ( drv ) {
		Com_Printf( "indep: video driver %s -> x11 (XWayland): Wayland cannot place windows\n", drv );
		return qtrue;
	}
	return qfalse;
}


/*
==================
Sys_SplitQuietWindow

qtrue = the game window about to be created must not take the focus
(--child / --noactivate, or a re-tile of a window that did not have it): it
is created hidden and shown by Sys_SplitShowWindow.
==================
*/
qboolean Sys_SplitQuietWindow( void ) {
	if ( !CL_SplitWindowRect( NULL, NULL, NULL, NULL ) && !( splitFlags & SPLIT_FLAG_NOACTIVATE ) ) {
		return qfalse;
	}
	if ( splitFlags & SPLIT_FLAG_NOACTIVATE ) {
		return qtrue;
	}
	return ( splitReshow && !splitReshowActive ) ? qtrue : qfalse;
}


// libX11, loaded at first use (SDL's X11 driver has it loaded already)
typedef Atom ( *pXInternAtom_t )( Display *, const char *, Bool );
typedef int ( *pXChangeProperty_t )( Display *, Window, Atom, Atom, int, int, const unsigned char *, int );
typedef int ( *pXLowerWindow_t )( Display *, Window );
typedef int ( *pXFlush_t )( Display * );

static struct {
	qboolean			tried;
	void				*lib;
	pXInternAtom_t		InternAtom;
	pXChangeProperty_t	ChangeProperty;
	pXLowerWindow_t		LowerWindow;
	pXFlush_t			Flush;
} x11;

static qboolean Split_LoadX11( void ) {
	if ( !x11.tried ) {
		x11.tried = qtrue;
		x11.lib = dlopen( "libX11.so.6", RTLD_NOW | RTLD_LOCAL );
		if ( x11.lib ) {
			x11.InternAtom = (pXInternAtom_t)dlsym( x11.lib, "XInternAtom" );
			x11.ChangeProperty = (pXChangeProperty_t)dlsym( x11.lib, "XChangeProperty" );
			x11.LowerWindow = (pXLowerWindow_t)dlsym( x11.lib, "XLowerWindow" );
			x11.Flush = (pXFlush_t)dlsym( x11.lib, "XFlush" );
		}
		if ( !x11.InternAtom || !x11.ChangeProperty || !x11.LowerWindow || !x11.Flush ) {
			Com_Printf( S_COLOR_YELLOW "indep: libX11.so.6 not usable: windows are shown normally\n" );
			x11.InternAtom = NULL;
		}
	}
	return x11.InternAtom ? qtrue : qfalse;
}


/*
==================
Sys_SplitShowWindow

sdl_glimp.c, after the window was created: show it.  A quiet window (created
hidden) gets _NET_WM_USER_TIME 0 before it is mapped -- the window manager
then maps it without giving it the focus -- and SDL is told not to activate
it; --noactivate test runs also put it below the other windows.  It starts
inactive (no mouse grab) until clicked.
==================
*/
void Sys_SplitShowWindow( void ) {
	qboolean quiet = Sys_SplitQuietWindow();
	SDL_SysWMinfo wm;
	long zero = 0;

	splitReshow = qfalse;
	if ( !SDL_window || !( SDL_GetWindowFlags( SDL_window ) & SDL_WINDOW_HIDDEN ) ) {
		return;
	}
	if ( !quiet ) {
		SDL_ShowWindow( SDL_window );
		return;
	}
	Com_Memset( &wm, 0, sizeof( wm ) );
	SDL_VERSION( &wm.version );
	if ( SDL_GetWindowWMInfo( SDL_window, &wm ) && wm.subsystem == SDL_SYSWM_X11 && Split_LoadX11() ) {
		x11.ChangeProperty( wm.info.x11.display, wm.info.x11.window, x11.InternAtom( wm.info.x11.display, "_NET_WM_USER_TIME", False ),
			XA_CARDINAL, 32, PropModeReplace, (const unsigned char *)&zero, 1 );
		x11.Flush( wm.info.x11.display );
	} else {
		Com_DPrintf( "indep: no X11 window: shown without _NET_WM_USER_TIME\n" );
	}
	SDL_SetHint( SDL_HINT_WINDOW_NO_ACTIVATION_WHEN_SHOWN, "1" );	// SDL2
	SDL_SetHint( "SDL_WINDOW_ACTIVATE_WHEN_SHOWN", "0" );			// SDL3 (sdl2-compat)
	SDL_ShowWindow( SDL_window );
	SDL_SetHint( SDL_HINT_WINDOW_NO_ACTIVATION_WHEN_SHOWN, "0" );
	SDL_SetHint( "SDL_WINDOW_ACTIVATE_WHEN_SHOWN", "1" );
	if ( ( splitFlags & SPLIT_FLAG_BOTTOM ) && wm.subsystem == SDL_SYSWM_X11 && x11.InternAtom ) {
		x11.LowerWindow( wm.info.x11.display, wm.info.x11.window );
		x11.Flush( wm.info.x11.display );
	}
	gw_active = qfalse;
}

#endif // __linux__
