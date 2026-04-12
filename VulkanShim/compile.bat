set NDK=C:\Users\Nick\AppData\Local\Android\Sdk\ndk\26.1.10909125
set TOOLCHAIN=%NDK%\toolchains\llvm\prebuilt\windows-x86_64\bin

%TOOLCHAIN%\aarch64-linux-android26-clang.cmd ^
    -shared ^
    -fPIC ^
    -O2 ^
    -fvisibility=default ^
    -o libvulkad.so ^
    vulkan_shim.c ^
    -ldl ^
    -llog ^
    -I%NDK%\toolchains\llvm\prebuilt\windows-x86_64\sysroot\usr\include
	