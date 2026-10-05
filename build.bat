@echo off
setlocal
cd /d %~dp0

REM 优先用已激活的环境，否则加载 emsdk
where emcc >nul 2>nul
if errorlevel 1 (
    if exist "E:\spj\emsdk\emsdk_env.bat" (
        echo [build] 加载 emsdk 环境...
        call "E:\spj\emsdk\emsdk_env.bat" >nul
    )
)

where emcc >nul 2>nul
if errorlevel 1 (
    echo [错误] 找不到 emcc
    exit /b 1
)

if not exist web mkdir web

echo [build] 编译 src\coax.cpp -^> web\coax.wasm
emcc -O3 -msimd128 -fno-exceptions -fno-rtti ^
  src\coax.cpp -o web\coax.js ^
  -s EXPORTED_FUNCTIONS="['_coax_encode_channel','_coax_process_preload','_coax_create','_coax_destroy','_coax_process','_coax_width','_coax_height','_coax_jit','_coax_spl','_coax_tx','_coax_rx','_malloc','_free']" ^
  -s EXPORTED_RUNTIME_METHODS="['HEAPU8','HEAPF32']" ^
  -s ALLOW_MEMORY_GROWTH=1 ^
  -s MODULARIZE=1 -s EXPORT_NAME=CoaxModule

if errorlevel 1 ( echo [错误] 编译失败 & exit /b 1 )
echo [完成] web\coax.js + web\coax.wasm
endlocal