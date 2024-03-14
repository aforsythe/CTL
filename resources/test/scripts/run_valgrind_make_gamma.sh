#!/bin/bash

set -x
set -u 

valgrind -s --error-exitcode=1 --leak-check=full --track-origins=yes --show-leak-kinds=all ../build/ctlrender/ctlrender -force -ctl ../resources/test/ctl/make_gamma.ctl ../unittest/ctlrender/colorbars_nuke_rgb_exr16.exr out.exr

