#!/usr/bin/make -f
# Makefile — PostmodularAccessibleUI (Strada 2)
#
# Uso in WSL (dopo aver installato wxWidgets con ~/wx-src/build-wx.sh):
#   cd ~/Cardinal/src/CardinalAccessibleUI
#   make
#
# Output: PostmodularAccessibleUI.exe (da copiare accanto a PostmodularNative.exe)

NAME     = PostmodularAccessibleUI
CXX      = x86_64-w64-mingw32-g++
WINDRES  = x86_64-w64-mingw32-windres

# wxWidgets cross-compilato per MinGW — path installato da build-wx.sh
WX_PREFIX ?= $(HOME)/wx32-mingw
WX_CONFIG  = $(WX_PREFIX)/bin/wx-config

# Flags base
CXXFLAGS  = -std=c++17 -O2 -DUNICODE -D_UNICODE -DWIN32
CXXFLAGS += -DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0A00
CXXFLAGS += $(shell $(WX_CONFIG) --cxxflags)

# Librerie: wx + Windows essenziali
WX_LIBS  = $(shell $(WX_CONFIG) --libs core,base)
EXTRA_LIBS = -lole32 -loleaut32 -luuid -lws2_32 -lwinmm -lwinspool -lcomdlg32 \
             -lcomctl32 -lgdi32 -ladvapi32 -lshell32 -luser32 -lkernel32

# Include paths: httplib + nlohmann/json (nella stessa directory)
INCS  = -I../extra      # httplib.h
INCS += -I.             # json.hpp

# Source is edited on the Windows side; sync it here before building.
WIN_SRC = /mnt/c/Users/Luca\ Casarotti/Documents/Cardinal/src/CardinalAccessibleUI/PostmodularAccessibleUI.cpp
SRCS = PostmodularAccessibleUI.cpp
OBJS = $(SRCS:.cpp=.o)

all: $(NAME).exe

# Sync from Windows side: teach Make that PostmodularAccessibleUI.cpp comes from WIN_SRC.
# This way 'make' always works even after 'make clean'.
PostmodularAccessibleUI.cpp: $(WIN_SRC)
	cp $(WIN_SRC) PostmodularAccessibleUI.cpp

sync: PostmodularAccessibleUI.cpp

$(NAME).exe: $(OBJS)
	$(CXX) -mwindows -static-libgcc -static-libstdc++ -o $@ $^ $(WX_LIBS) $(EXTRA_LIBS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INCS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(NAME).exe

.PHONY: all sync clean

