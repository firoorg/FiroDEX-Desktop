Set-ExecutionPolicy RemoteSigned -scope CurrentUser

<<<<<<< HEAD


=======
>>>>>>> a21448ddc0da1e6353734fe7a5914fbe8b871c7d
Invoke-Expression (New-Object System.Net.WebClient).DownloadString('https://get.scoop.sh')
scoop install llvm --global
scoop install ninja --global
scoop install cmake@3.22.0 --global
scoop install git --global
scoop install 7zip  --global
scoop cache rm 7zip
scoop cache rm git
scoop cache rm cmake
scoop cache rm ninja
scoop cache rm llvm
$Env:QT_INSTALL_CMAKE_PATH = "C:\Qt\$Env:QT_VERSION\msvc2019_64"
$Env:QT_ROOT = "C:\Qt"
mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Release -GNinja ../ 
ninja
ninja install
