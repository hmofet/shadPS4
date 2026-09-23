@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
set "PATH=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\Llvm\x64\bin;%PATH%"
cd /d C:\Users\arinb\Documents\GitHub\shadPS4
echo === emulator ===
cmake --build Build\x64-Clang-RelWithDebInfo || exit /b 2
echo === tests ===
cmake --build Build\x64-Clang-Tests --target shadps4_settings_test || exit /b 3
echo === BUILD OK ===
