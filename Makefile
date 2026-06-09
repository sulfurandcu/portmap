# Makefile for PortMap Tool
# Requires MinGW GCC compiler

CC = d:\develop.r.environment\msys2\mingw64\bin\gcc
WINDRES = d:\develop.r.environment\msys2\mingw64\bin\windres

CFLAGS = -O2 -Wall -D_CRT_SECURE_NO_WARNINGS -finput-charset=UTF-8 -fexec-charset=GBK
GUI_CFLAGS = $(CFLAGS) -D_GUI_BUILD -mwindows
LIBS = -lws2_32 -lcomctl32 -lshell32 -lgdi32 -luxtheme

# Source files
SOURCES = portmap_cli.c portmap_core.c
GUI_SOURCES = portmap_gui.c portmap_core.c
HEADERS = portmap.h resource.h
RC_SOURCE = portmap.rc
RC_OBJ = portmap.o

# Output files
TARGET = portmap_cli.exe
GUI_TARGET = portmap.exe

# Default target
all: $(TARGET) $(GUI_TARGET)

# Compile resource file
$(RC_OBJ): $(RC_SOURCE)
	$(WINDRES) -i $(RC_SOURCE) -o $(RC_OBJ)

# Command-line version
$(TARGET): $(SOURCES) $(HEADERS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SOURCES) $(LIBS)

# GUI version (includes resource file for icon)
$(GUI_TARGET): $(GUI_SOURCES) $(HEADERS) $(RC_OBJ)
	$(CC) $(GUI_CFLAGS) -o $(GUI_TARGET) $(GUI_SOURCES) $(RC_OBJ) $(LIBS)

# Clean build artifacts
clean:
	@echo Cleaning build artifacts...
	@rm -f *.o *.exe 2>/dev/null || true

# Install (copy to system directory)
install: $(TARGET) $(GUI_TARGET)
	@echo Installing executables...
	@mkdir -p "$(HOME)/bin" 2>/dev/null || true
	@cp $(TARGET) "$(HOME)/bin/" 2>/dev/null || copy $(TARGET) "%USERPROFILE%\bin\" /Y
	@cp $(GUI_TARGET) "$(HOME)/bin/" 2>/dev/null || copy $(GUI_TARGET) "%USERPROFILE%\bin\" /Y

# Uninstall
uninstall:
	@echo Uninstalling executables...
	@rm -f "$(HOME)/bin/$(TARGET)" 2>/dev/null || del /Q "%USERPROFILE%\bin\$(TARGET)" 2>nul
	@rm -f "$(HOME)/bin/$(GUI_TARGET)" 2>/dev/null || del /Q "%USERPROFILE%\bin\$(GUI_TARGET)" 2>nul

# Help
help:
	@echo Available targets:
	@echo   all           - Build both command-line and GUI versions
	@echo   cli           - Build command-line version only
	@echo   gui           - Build GUI version only
	@echo   portmap.exe   - Build command-line version only
	@echo   portmap_gui.exe - Build GUI version only
	@echo   clean         - Remove build artifacts
	@echo   install       - Install executables to user bin directory
	@echo   uninstall     - Remove executables from user bin directory
	@echo   help          - Show this help message

# Individual targets for convenience
portmap.exe: $(TARGET)
portmap_gui.exe: $(GUI_TARGET)
cli: $(TARGET)
gui: $(GUI_TARGET)

.PHONY: all clean install uninstall help portmap.exe portmap_gui.exe cli gui
