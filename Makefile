# DSiDL - homebrew downloader for Nintendo DSi (BlocksDS)

BLOCKSDS	?= /opt/blocksds/core
BLOCKSDSEXT	?= /opt/blocksds/external

APP_VERSION	?= 1.0.4

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

include $(BLOCKSDS)/sys/default_makefiles/rom_arm9/Makefile
