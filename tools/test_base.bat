@echo off
set "ROOT=%~dp0.."
pushd "%ROOT%"
engine\build\tts_engine_cli.exe -m models -t "Hello from the engine layer." -o examples\engine_test.wav
popd
