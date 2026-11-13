The remastered assets go here. Extract each asset pack (fe1 and/or fe2) into the repository folder, the one
with install.bat: the archive's own data folder merges with this one, giving data\fe1\... or data\fe2\...
plus data\manifests\ and data\build\. install.bat checks every file against data\manifests\<game>.json.
install.bat also puts ffmpeg.exe (the movie decoder) and the fonts here.
