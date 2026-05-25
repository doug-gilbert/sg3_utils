#!/bin/sh

echo "Entering lib/cmake_del_artifacts.sh"

rm -rf \
	CMakeFiles \
	cmake_install.cmake \
	sgutils2Config.cmake \
	sgutils2ConfigVersion.cmake \
	Makefile

