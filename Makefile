# SPDX-License-Identifier: GPL-2.0

# Configuration and compile options for standalone module version in a separate
# file. The upstream version should contains the configuration in the Kconfig
# file, and should be free from all branches of conditional compilation.
include ${M}/Makefile-standalone

sblkdev-y := main.o device.o
obj-$(CONFIG_SBLKDEV) += sblkdev.o

# Set the below line to enable debug messages to show up in the kernel log
# Tip: 'journalctl -k -f -u sblkdev' will show the kernel log messages related to sblkdev in real time
# Comment out the below line to disable debug messages
ccflags-y += -DDEBUG