@echo off
rem 主机单元测试构建/运行脚本（Windows，需要 gcc 在 PATH 中）
rem 用法：build_run.bat   （在 test\host 目录下执行）
setlocal
set CORE=..\..\main\core
set OUT=build
if not exist %OUT% mkdir %OUT%

gcc -std=c11 -Wall -Wextra -Wpedantic -O1 -g ^
    -I%CORE% -I. ^
    "%CORE%\pillbox.c" "%CORE%\dose_verify.c" "%CORE%\schedule.c" "%CORE%\stock.c" ^
    mock_hal.c test_dose_verify.c test_schedule.c test_session.c test_safety.c main.c ^
    -o "%OUT%\pbtest.exe"
if errorlevel 1 goto :fail

"%OUT%\pbtest.exe"
if errorlevel 1 goto :fail
echo.
echo [OK] 测试全部通过
goto :eof

:fail
echo.
echo [FAIL] 构建或测试失败
exit /b 1
