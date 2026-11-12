@echo off
REM Compiles the GXM shaders on Windows: runs the variant generator, psp2cgc on
REM every .cg in src\render\shaders\cg and cg\gxp (the .gxp goes to cg\gxp),
REM then records the source of each .gxp for the stale check (the Vita build
REM runs it). Run it before a Vita build that changed a shader.
setlocal
set "ROOT=%~dp0.."
set "CG=%ROOT%\src\render\shaders\cg"
set "PSP2CGC=C:\vitasdk-tools\psp2cgc.exe"
if not exist "%PSP2CGC%" (
    echo psp2cgc not found: %PSP2CGC%
    exit /b 1
)
python "%ROOT%\tools\gen_gxm_shaders.py"
if errorlevel 1 exit /b 1
set FAILED=0
for %%f in ("%CG%\*.cg" "%CG%\gxp\*.cg") do call :compile "%%~f" "%%~nf"
if %FAILED% neq 0 (
    echo compile_gxm_shaders: a shader did not compile
    exit /b 1
)
python "%ROOT%\tools\check_gxp_stale.py" --record
exit /b %errorlevel%

REM A name that ends in _v is a vertex program, any other a fragment program.
:compile
set "NAME=%~2"
set PROFILE=sce_fp_psp2
if "%NAME:~-2%"=="_v" set PROFILE=sce_vp_psp2
echo %NAME%
"%PSP2CGC%" -profile %PROFILE% -Wperf "-I%CG%" "%~1" -o "%CG%\gxp\%NAME%.gxp"
if errorlevel 1 set FAILED=1
exit /b 0
