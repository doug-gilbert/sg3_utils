#!/bin/sh

set -x

./tst_sg_lib --help
echo "\n"

./tst_sg_lib --byteswap=16 --num=100m
./tst_sg_lib --byteswap=32 --num=100m
./tst_sg_lib --byteswap=64 --num=100m
echo "\n"

./tst_sg_lib --exit --verbose
echo "\n"

./tst_sg_lib --hex2
echo "\n"

./tst_sg_lib --printf
echo "\n"

./tst_sg_lib --sense
./tst_sg_lib --sense --leadin=">>> "
echo "\n"

./tst_sg_lib --unaligned
