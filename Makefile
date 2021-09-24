.PHONY: mipsel arm

mipsel:
	/root/asuswrt/tools/brcm/hndtools-mipsel-uclibc/bin/mipsel-linux-uclibc-gcc tinysdb.c -o tinysdb-mipsel
	
arm:
	STAGING_DIR=/root/asuswrt/tools/openwrt-gcc463.arm /root/asuswrt/tools/openwrt-gcc463.arm/bin/arm-openwrt-linux-uclibcgnueabi-gcc tinysdb.c -o tinysdb-arm