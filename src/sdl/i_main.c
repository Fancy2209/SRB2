// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// Copyright (C) 1993-1996 by id Software, Inc.
// Copyright (C) 1998-2000 by DooM Legacy Team.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//-----------------------------------------------------------------------------
/// \file
/// \brief Main program, simply calls D_SRB2Main and D_SRB2Loop, the high level loop.

#include "../doomdef.h"
#include "../m_argv.h"
#include "../d_main.h"
#include "../i_system.h"

#ifdef __GNUC__
#include <unistd.h>
#endif

#ifdef _WII
#include <limits.h>
#include <network.h>
#include <fat.h>
#include <wiisocket.h>
#include <gctypes.h>
#include <gccore.h>
#ifdef REMOTE_DEBUGGING
#include <debug.h>
#endif
static char wiicwd[PATH_MAX] = "sd:/";
#endif

#ifdef HAVE_SDL

#ifdef HAVE_TTF
#include "SDL.h"
#include "i_ttf.h"
#endif

#if defined (_WIN32) && !defined (main)
//#define SDLMAIN
#endif

#ifdef SDLMAIN
#include "SDL_main.h"
#elif defined(FORCESDLMAIN)
extern int SDL_main(int argc, char *argv[]);
#endif

#ifdef LOGMESSAGES
FILE *logstream = NULL;
#endif

#ifndef DOXYGEN
#ifndef O_TEXT
#define O_TEXT 0
#endif

#ifndef O_SEQUENTIAL
#define O_SEQUENTIAL 0
#endif
#endif

#if defined (_WIN32)
#include "../win32/win_dbg.h"
typedef BOOL (WINAPI *p_IsDebuggerPresent)(VOID);
#endif

#if defined (_WIN32)
static inline VOID MakeCodeWritable(VOID)
{
#ifdef USEASM // Disable write-protection of code segment
	DWORD OldRights;
	const DWORD NewRights = PAGE_EXECUTE_READWRITE;
	PBYTE pBaseOfImage = (PBYTE)GetModuleHandle(NULL);
	PIMAGE_DOS_HEADER dosH =(PIMAGE_DOS_HEADER)pBaseOfImage;
	PIMAGE_NT_HEADERS ntH = (PIMAGE_NT_HEADERS)(pBaseOfImage + dosH->e_lfanew);
	PIMAGE_OPTIONAL_HEADER oH = (PIMAGE_OPTIONAL_HEADER)
		((PBYTE)ntH + sizeof (IMAGE_NT_SIGNATURE) + sizeof (IMAGE_FILE_HEADER));
	LPVOID pA = pBaseOfImage+oH->BaseOfCode;
	SIZE_T pS = oH->SizeOfCode;
#if 1 // try to find the text section
	PIMAGE_SECTION_HEADER ntS = IMAGE_FIRST_SECTION (ntH);
	WORD s;
	for (s = 0; s < ntH->FileHeader.NumberOfSections; s++)
	{
		if (memcmp (ntS[s].Name, ".text\0\0", 8) == 0)
		{
			pA = pBaseOfImage+ntS[s].VirtualAddress;
			pS = ntS[s].Misc.VirtualSize;
			break;
		}
	}
#endif

	if (!VirtualProtect(pA,pS,NewRights,&OldRights))
		I_Error("Could not make code writable\n");
#endif
}
#endif


/**	\brief	The main function

	\param	argc	number of arg
	\param	*argv	string table

	\return	int
*/
#if defined (__GNUC__) && (__GNUC__ >= 4)
#pragma GCC diagnostic ignored "-Wmissing-noreturn"
#endif

#ifdef FORCESDLMAIN
int SDL_main(int argc, char **argv)
#else
int main(int argc, char **argv)
#endif
{
	const char *logdir = NULL;
	myargc = argc;
	myargv = argv; /// \todo pull out path to exe from this string

#ifdef HAVE_TTF
#ifdef _WIN32
	I_StartupTTF(FONTPOINTSIZE, SDL_INIT_VIDEO|SDL_INIT_AUDIO, SDL_SWSURFACE);
#else
	I_StartupTTF(FONTPOINTSIZE, SDL_INIT_VIDEO, SDL_SWSURFACE);
#endif
#endif

// init Wii-specific stuff
#ifdef _WII
#ifdef REMOTE_DEBUGGING
#if REMOTE_DEBUGGING == 0
	DEBUG_Init(GDBSTUB_DEVICE_TCP, GDBSTUB_DEF_TCPPORT); // Port 2828
#elif REMOTE_DEBUGGING > 2
	DEBUG_Init(GDBSTUB_DEVICE_TCP, REMOTE_DEBUGGING); // Custom Port
#elif REMOTE_DEBUGGING < 0
	DEBUG_Init(GDBSTUB_DEVICE_USB, GDBSTUB_DEF_CHANNEL); // Slot 1
#else
	DEBUG_Init(GDBSTUB_DEVICE_USB, REMOTE_DEBUGGING-1); // Custom Slot
#endif
#endif
	// Start FAT filesystem
	fatInitDefault();

	if (getcwd(wiicwd, PATH_MAX))
		putenv("HOME=/SRB2Wii/");
	//SYS_STDIO_Report(true);
#endif

	logdir = D_Home();

#ifdef LOGMESSAGES
#ifdef _WII
		logstream = fopen(va("%s/srb2log.txt",logdir), "a");
#elif defined(DEFAULTDIR)
	if (logdir)
		logstream = fopen(va("%s/"DEFAULTDIR"/log.txt",logdir), "wt");
	else
		logstream = fopen(va("%s/srb2log.txt",logdir), "a");
#endif
		logstream = fopen("./log.txt", "wt");
#endif
	//I_OutputMsg("I_StartupSystem() ...\n");
	I_StartupSystem();
	#ifdef _WII
	// Credits to Andrew Piroli
		// try a few times to initialize libwiisocket (?)
	int socket_init_success = -1;
	for (int attempts = 0;attempts < 20;attempts++) {
		socket_init_success = wiisocket_init();
		CONS_Printf("attempt: %d wiisocket_init: %d\n", attempts, socket_init_success);
		if (socket_init_success == 0)
			break;
	}
	if (socket_init_success != 0) {
		puts("failed to init wiisocket");
	}
	// try a few times to get an ip (?)
	u32 ip = 0;
	for (int attempts = 0; attempts < 20; attempts++) {
		ip = gethostid();
		CONS_Printf("attempt: %d gethostid: %x\n", attempts, ip);
		if (ip)
			break;
	}
	if (!ip) {
		puts("failed to get ip");
	}
	#endif
#if defined (_WIN32)
	{
#if 0 // just load the DLL
		p_IsDebuggerPresent pfnIsDebuggerPresent = (p_IsDebuggerPresent)GetProcAddress(GetModuleHandleA("kernel32.dll"), "IsDebuggerPresent");
		if ((!pfnIsDebuggerPresent || !pfnIsDebuggerPresent())
#ifdef BUGTRAP
			&& !InitBugTrap()
#endif
			)
#endif
		{
			LoadLibraryA("exchndl.dll");
		}
	}
#ifndef __MINGW32__
	prevExceptionFilter = SetUnhandledExceptionFilter(RecordExceptionInfo);
#endif
	MakeCodeWritable();
#endif

	// startup SRB2
	CONS_Printf("Setting up SRB2...\n");
	D_SRB2Main();
	CONS_Printf("Entering main game loop...\n");
	// never return
	D_SRB2Loop();

#ifdef BUGTRAP
	// This is safe even if BT didn't start.
	ShutdownBugTrap();
#endif

	// return to OS
	return 0;
}
#endif
