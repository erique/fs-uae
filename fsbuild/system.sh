# Intended for sourcing by other shell scripts
# This file is automatically generated by fs-package

case "`uname`" in
    Linux*)  SYSTEM_OS=Linux;;
    Darwin*) SYSTEM_OS=macOS;;
    MINGW*)  SYSTEM_OS=Windows;;
    *)       SYSTEM_OS=Unknown;;
esac

case "`uname -m`" in
    x86_64*) SYSTEM_ARCH=x86-64;;
    arm64*)  SYSTEM_ARCH=ARM64;;
    *)       SYSTEM_ARCH=Unknown;;
esac

if [ $SYSTEM_OS = "Windows" ]; then
SYSTEM_EXE=.exe
SYSTEM_DLL=.dll
else
SYSTEM_EXE=
SYSTEM_DLL=.so
fi

# FIXME: Deprecated alias
SYSTEM=$SYSTEM_OS

if [ "$SYSTEM_OS_DIST" = "" ]; then
SYSTEM_OS_DIST=$SYSTEM_OS
fi

if [ "$SYSTEM_ARCH_DIST" = "" ]; then
SYSTEM_ARCH_DIST=$SYSTEM_ARCH
fi