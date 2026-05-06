# Microsoft Developer Studio Project File - Name="mcp-w32s" - Package Owner=<4>
# Microsoft Developer Studio Generated Build File, Format Version 6.00
# ** DO NOT EDIT **

# TARGTYPE "Win32 (x86) Console Application" 0x0103

CFG=mcp-w32s - Win32 Debug
!MESSAGE This is not a valid makefile. To build this project using NMAKE,
!MESSAGE use the Export Makefile command and run
!MESSAGE
!MESSAGE NMAKE /f "mcp-w32s.mak".
!MESSAGE
!MESSAGE You can specify a configuration when running NMAKE
!MESSAGE by defining the macro CFG on the command line. For example:
!MESSAGE
!MESSAGE NMAKE /f "mcp-w32s.mak" CFG="mcp-w32s - Win32 Debug"
!MESSAGE
!MESSAGE Possible choices for configuration are:
!MESSAGE
!MESSAGE "mcp-w32s - Win32 Release" (based on "Win32 (x86) Console Application")
!MESSAGE "mcp-w32s - Win32 Debug" (based on "Win32 (x86) Console Application")
!MESSAGE

# Begin Project
# PROP AllowPerConfigDependencies 0
# PROP Scc_ProjName ""
# PROP Scc_LocalPath ""
CPP=cl.exe
RSC=rc.exe

!IF  "$(CFG)" == "mcp-w32s - Win32 Release"

# PROP BASE Use_MFC 0
# PROP BASE Use_Debug_Libraries 0
# PROP BASE Output_Dir "Release"
# PROP BASE Intermediate_Dir "Release"
# PROP BASE Target_Dir ""
# PROP Use_MFC 0
# PROP Use_Debug_Libraries 0
# PROP Output_Dir "Release"
# PROP Intermediate_Dir "Release"
# PROP Ignore_Export_Lib 0
# PROP Target_Dir ""
# ADD BASE CPP /nologo /W3 /O2 /D "WIN32" /D "NDEBUG" /D "_CONSOLE" /YX /FD /c
# ADD CPP /nologo /W3 /O2 /I "..\src" /D "WIN32" /D "NDEBUG" /D "_CONSOLE" /TC /G3 /YX /FD /c
# ADD BASE RSC /l 0x409 /d "NDEBUG"
# ADD RSC /l 0x409 /d "NDEBUG"
BSC32=bscmake.exe
# ADD BASE BSC32 /nologo
# ADD BSC32 /nologo
LINK32=link.exe
# ADD BASE LINK32 kernel32.lib user32.lib /nologo /subsystem:console /machine:I386
# ADD LINK32 kernel32.lib user32.lib wsock32.lib /nologo /subsystem:console /machine:I386 /out:"Release/mcp-w32s.exe" /FIXED:NO /BASE:0x10000

!ELSEIF  "$(CFG)" == "mcp-w32s - Win32 Debug"

# PROP BASE Use_MFC 0
# PROP BASE Use_Debug_Libraries 1
# PROP BASE Output_Dir "Debug"
# PROP BASE Intermediate_Dir "Debug"
# PROP BASE Target_Dir ""
# PROP Use_MFC 0
# PROP Use_Debug_Libraries 1
# PROP Output_Dir "Debug"
# PROP Intermediate_Dir "Debug"
# PROP Ignore_Export_Lib 0
# PROP Target_Dir ""
# ADD BASE CPP /nologo /W3 /Zi /Od /D "WIN32" /D "_DEBUG" /D "_CONSOLE" /YX /FD /c
# ADD CPP /nologo /W3 /Zi /Od /I "..\src" /D "WIN32" /D "_DEBUG" /D "_CONSOLE" /TC /G3 /YX /FD /c
# ADD BASE RSC /l 0x409 /d "_DEBUG"
# ADD RSC /l 0x409 /d "_DEBUG"
BSC32=bscmake.exe
# ADD BASE BSC32 /nologo
# ADD BSC32 /nologo
LINK32=link.exe
# ADD BASE LINK32 kernel32.lib user32.lib /nologo /subsystem:console /debug /machine:I386
# ADD LINK32 kernel32.lib user32.lib wsock32.lib /nologo /subsystem:console /debug /machine:I386 /out:"Debug/mcp-w32s.exe" /FIXED:NO /BASE:0x10000

!ENDIF

# Begin Target

# Name "mcp-w32s - Win32 Release"
# Name "mcp-w32s - Win32 Debug"
# Begin Group "Source Files"

# PROP Default_Filter "cpp;c;cxx;rc;def;r;odl;idl;hpj;bat"
# Begin Source File

SOURCE=..\src\mcp-w32s.c
# End Source File
# Begin Source File

SOURCE=..\src\serial.c
# End Source File
# Begin Source File

SOURCE=..\src\json_parser.c
# End Source File
# Begin Source File

SOURCE=..\src\base64.c
# End Source File
# Begin Source File

SOURCE=..\src\file_ops.c
# End Source File
# End Group
# Begin Group "Header Files"

# PROP Default_Filter "h;hpp;hxx;hm;inl"
# Begin Source File

SOURCE=..\src\common.h
# End Source File
# Begin Source File

SOURCE=..\src\json_parser.h
# End Source File
# Begin Source File

SOURCE=..\src\serial.h
# End Source File
# Begin Source File

SOURCE=..\src\base64.h
# End Source File
# Begin Source File

SOURCE=..\src\file_ops.h
# End Source File
# End Group
# End Target
# End Project
