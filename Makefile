# DSiDL - homebrew downloader for Nintendo DSi (BlocksDS)

BLOCKSDS	?= /opt/blocksds/core
BLOCKSDSEXT	?= /opt/blocksds/external

APP_VERSION	?= 1.2.1

NAME		:= dsidl
GAME_TITLE	:= DSiDL
GAME_SUBTITLE	:= Homebrew downloader
GAME_AUTHOR	:= smikkelvriend
GAME_ICON	:= icon.bmp

SOURCEDIRS	:= source
BINDIRS		:= data

DEFINES		:= -DAPP_VERSION=\"$(APP_VERSION)\" \
		   -DMBEDTLS_USER_CONFIG_FILE=\"nds_mbedtls_config.h\"

ARM7ELF		:= $(BLOCKSDS)/sys/arm7/main_core/arm7_dswifi_maxmod.elf

LIBS		:= -ldswifi9 -lnds9 -lmbedtls_nds
LIBDIRS		:= $(BLOCKSDS)/libs/dswifi \
		   $(BLOCKSDS)/libs/libnds \
		   $(CURDIR)/mbedtls-nds

# Embed the phone web page (web/index.html) as data/webui.bin
$(shell mkdir -p data && cp -u web/index.html data/webui.bin)

include $(BLOCKSDS)/sys/default_makefiles/rom_arm9/Makefile
