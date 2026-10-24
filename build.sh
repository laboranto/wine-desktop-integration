#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# mingw 없이 clang + lld 로 빌드
set -eu
cd "$(dirname "$0")"
CC="clang --target=x86_64-windows-gnu -O2 -nostdlib -fuse-ld=lld -DWIN32_LEAN_AND_MEAN -Wno-pragma-pack -Istub -I/usr/include/wine/windows -L/usr/lib/wine/x86_64-windows"
$CC -shared -o comdlg32.dll comdlg32.c comdlg32.def -Wl,-e,DllMain -lntdll -lkernel32 -luser32
$CC -o explorer.exe explorer.c -Wl,-e,start -Wl,--subsystem,windows -lntdll -lkernel32
$CC -o test/test.exe test/test.c -Wl,-e,start -Wl,--subsystem,console -lntdll -lkernel32 -luser32 -lcomdlg32 -lshell32
