#
# Makefile options for unices (linux, bsd...)
#

EXENAME?=lsdl2srb2
ifndef WIIU

opts+=-DUNIXCOMMON -DLUA_USE_POSIX
# Use -rdynamic so a backtrace log shows function names
# instead of addresses
libs+=-lm -rdynamic

ifndef NOHW
opts+=-I/usr/X11R6/include
libs+=-L/usr/X11R6/lib
endif
endif

SDL?=1

ifdef WIIU
system=WiiU
UNIX=1
LINUX=0
NONX86=1
EXENAME=lsdl2srb2.elf
DBGNAME=lsdl2srb2.debug.elf
NOSDLMAIN=1
NOHW=1
ECHO=1
#For now no Online
NONET=1
#Until I figure audio out
NOMIXER=1
NOGME=1
NOOPENMPT=1

CC=/opt/devkitpro/devkitPPC/bin/powerpc-eabi-gcc
PKG_CONFIG=/opt/devkitpro/portlibs/ppc/bin/powerpc-eabi-pkg-config 
SDL_CONFIG=/opt/devkitpro/portlibs/wiiu/bin/sdl2-config
PNG_CONFIG=/opt/devkitpro/portlibs/ppc/bin/libpng-config
opts+=-Wall $(MACHDEP) -DUNIXCOMMON -I$(WUT_ROOT)/include -I$(DEVKITPRO)/portlibs/ppc/include -I$(DEVKITPRO)/portlibs/wiiu/include -ffunction-sections -D__WIIU__ -D__WUT__
libs+=$(ARCH) $(RPXSPECS) -L$(WUT_ROOT)/lib -L$(DEVKITPRO)/portlibs/ppc/lib -L$(DEVKITPRO)/portlibs/wiiu/lib -lwut 
endif

# In common usage.
ifndef WIIU
ifdef LINUX
libs+=-lrt
passthru_opts+=NOTERMIOS
endif
endif

# Tested by Steel, as of release 2.2.8.
ifdef FREEBSD
opts+=-I/usr/X11R6/include -DLINUX -DFREEBSD
libs+=-L/usr/X11R6/lib -lkvm -lexecinfo
endif

# FIXME: UNTESTED
#ifdef SOLARIS
#NOIPX=1
#opts+=-I/usr/local/include -I/opt/sfw/include \
#		-DSOLARIS -DINADDR_NONE=INADDR_ANY -DBSD_COMP
#libs+=-L/opt/sfw/lib -lsocket -lnsl
#endif
