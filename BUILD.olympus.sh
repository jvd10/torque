#!/bin/sh
# build steps
# on conputes add packages:
# boost boost-devel
# libxml2
# gcc-g++
# pam-devel

./configure --prefix=/opt --with-pam=yes --enable-blcr --enable-cpuset --enable-maxdefault
make -j8

# for some reason even after enabling pam it doesn't do "make && make install" in src/pam
pushd src/pam
make
popd


################################################################################################### INSTALL NOTES

# 1.) CPUSETS 
# 
# To configure cpuset
# 
# As root, mount the virtual filesystem for cpusets:
# mount -t cpuset none /dev/cpuset
# 
# Do this for each MOM that is to use cpusets.
# Because cpuset usage is a build-time option in TORQUE, you must add --enable-cpuset to your configure options:
# ./configure --enable-cpuset
# Use this configuration for the MOMs across your system.
# 
