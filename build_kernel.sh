#!/bin/bash

export ARCH=arm64
#export CROSS_COMPILE=/tools/prebuilts/gcc-cfp-jopp-only/aarch64-linux-android-4.9/bin/aarch64-linux-android-
export ANDROID_MAJOR_VERSION=o
#export CROSS_COMPILE=./tools/prebuilts/gcc-cfp-jopp-only/aarch64-linux-android-4.9/bin/aarch64-linux-android-
#export PATH=$(pwd)/./tools/prebuilts/gcc-cfp-jopp-only/aarch64-linux-android-4.9/bin:$PATH

make exynos8895-dreamltekor_kor_defconfig
make -j64
