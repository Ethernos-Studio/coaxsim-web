@echo off
cd /d %~dp0web
echo 打开浏览器访问 http://localhost:8000
python -m http.server 8000