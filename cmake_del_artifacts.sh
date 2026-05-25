#!/bin/sh

# Designed to remove 'cmake . ; cmake --build . ' artifacts from an in-tree
# build. For an out-of-tree build (e.g. 'cmake -S . -B build ; cd build ;
# cmake --build . ; cpack . ') simply do 'cd .. ; rm -rf build ' .

cd include || exit
./cmake_del_artifacts.sh
cd ..

cd lib || exit
./cmake_del_artifacts.sh
cd ..

cd src || exit
./cmake_del_artifacts.sh
cd ..

cd scripts || exit
./cmake_del_artifacts.sh
cd ..

cd doc || exit
./cmake_del_artifacts.sh
cd ..

rm -rf \
	build \
	CMakeCache.txt \
	CMakeFiles \
	CPackConfig.cmake \
	CPackSourceConfig.cmake \
	CMakeFiles \
	cmake_install.cmake \
	config.h \
	install_manifest.txt \
	_CPack_Packages \
	CTestTestfile.cmake \
	DartConfiguration.tcl \
	install_manifest_development.txt \
	install_manifest_runtime.txt \
	install_manifest_utilities.txt \
	Testing \
	Makefile

