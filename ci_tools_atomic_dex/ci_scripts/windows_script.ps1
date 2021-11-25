Set-ExecutionPolicy RemoteSigned -scope CurrentUser



Invoke-Expression (New-Object System.Net.WebClient).DownloadString('https://get.scoop.sh')
scoop install llvm --global
scoop install ninja --global
scoop install cmake@3.20.5 --global
scoop install git --global
scoop install 7zip  --global
scoop cache rm 7zip
scoop cache rm git
scoop cache rm cmake
scoop cache rm ninja
scoop cache rm llvm
scoop cache rm nim
$Env:QT_INSTALL_CMAKE_PATH = "C:\Qt\$Env:QT_VERSION\msvc2019_64"
$Env:QT_ROOT = "C:\Qt"
mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Release -GNinja ../ 
ninja
ninja install
