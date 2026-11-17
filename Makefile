#---------------------------------------------------------------
# Pipa Combate - Makefile (PSL1GHT)
# Gera: pipa_combate.elf -> EBOOT.BIN -> pipa_combate.pkg
#---------------------------------------------------------------

PS3DEV ?= /usr/local/ps3dev
PSL1GHT ?= $(PS3DEV)/psl1ght
include $(PSL1GHT)/ppu_rules

TARGET     := pipa_combate
PKG_DIR    := pkg
CONTENT_ID := UP0001-PIPA00001_00-PIPACOMBATE0001

SOURCES  := .
CFLAGS   := -O2 -Wall
LIBS     := -lsysutil -lio -lrsx -lsysmodule -lm -lc

all: $(TARGET).pkg

$(TARGET).elf: main.c
	$(PPU_CC) $(CFLAGS) -o $@ main.c $(LIBS)

EBOOT.BIN: $(TARGET).elf
	$(PS3DEV)/bin/make_self $< $@

$(PKG_DIR)/PS3_GAME/PARAM.SFO: sfo.xml
	mkdir -p $(PKG_DIR)/PS3_GAME $(PKG_DIR)/USRDIR
	$(PS3DEV)/bin/sfo.py -f sfo.xml $@

$(PKG_DIR)/PS3_GAME/ICON0.PNG:
	@test -f ICON0.PNG || (echo "Falta ICON0.PNG (320x176) na raiz do projeto" && exit 1)
	mkdir -p $(PKG_DIR)/PS3_GAME
	cp ICON0.PNG $@

$(TARGET).pkg: EBOOT.BIN $(PKG_DIR)/PS3_GAME/PARAM.SFO $(PKG_DIR)/PS3_GAME/ICON0.PNG
	cp EBOOT.BIN $(PKG_DIR)/USRDIR/EBOOT.BIN
	$(PS3DEV)/bin/pkg.py --contentid $(CONTENT_ID) $(PKG_DIR) $@

clean:
	rm -rf $(TARGET).elf EBOOT.BIN $(PKG_DIR) $(TARGET).pkg

.PHONY: all clean
