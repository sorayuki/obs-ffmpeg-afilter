curl -o obs-bin-x86.zip "https://cdn-fastly.obsproject.com/downloads/OBS-Studio-26.0.2-Full-x86.zip"
curl -o obs-bin-x64.zip "https://cdn-fastly.obsproject.com/downloads/OBS-Studio-26.0.2-Full-x64.zip"
curl -o deps.zip "https://obsproject.com/downloads/dependencies2017.zip"
git clone --depth=1 -b 26.0.2 "https://github.com/obsproject/obs-studio.git" obs-src
cmake -E make_directory obs-bin
pushd obs-bin
cmake -E tar zxf ..\obs-bin-x86.zip
cmake -E tar zxf ..\obs-bin-x64.zip
popd
cmake -E make_directory deps
pushd deps
cmake -E tar zxf ..\deps.zip
popd
cmake -G "Visual Studio 16 2019" -A Win32 -B build_x86 -S . -DCMAKE_INSTALL_PREFIX=dist
cmake --build build_x86 --config Release
cmake --install build_x86 --config Release
cmake -G "Visual Studio 16 2019" -A x64 -B build_x86 -S . -DCMAKE_INSTALL_PREFIX=dist
cmake --build build_x64 --config Release
cmake --install build_x64 --config Release

if not exist dist\nul exit /b
cd dist
cmake -E tar cf ..\release.zip --format=zip .
cd ..
