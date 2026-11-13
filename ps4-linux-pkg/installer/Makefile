# Generic PS4 Linux loader PKG (OpenOrbis). Run through ../make-pkg.sh, which stages the
# drop-in files into assets/linux/ and passes TITLE, VERSION, TITLE_ID, CONTENT_ID, ASSETS.
TITLE       ?= Linux for PS4
VERSION     ?= 1.00
TITLE_ID    ?= LNUX00001
CONTENT_ID  ?= IV0000-$(TITLE_ID)_00-PS4LINUXLOADER00
ASSETS      ?= $(wildcard assets/linux/*)

LIBS        := -lc -lkernel -lSceMsgDialog -lSceCommonDialog -lSceSysmodule -lSceSystemService -lSceNet

TOOLCHAIN   := $(OO_PS4_TOOLCHAIN)
PROJDIR     := installer
INTDIR      := build
CFILES      := $(wildcard $(PROJDIR)/*.c)
OBJS        := $(patsubst $(PROJDIR)/%.c, $(INTDIR)/%.o, $(CFILES))
CFLAGS      := --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -O2 -Wall -Wextra -Wno-unused-parameter \
               -c -isysroot $(TOOLCHAIN) -isystem $(TOOLCHAIN)/include \
               -DTITLE_ID='"$(TITLE_ID)"' -DAPP_TITLE='"$(TITLE)"' \
               $(EXTRA_CFLAGS)
LDFLAGS     := -m elf_x86_64 -pie --script $(TOOLCHAIN)/link.x --eh-frame-hdr -L$(TOOLCHAIN)/lib $(LIBS) $(TOOLCHAIN)/lib/crt1.o
CC          := clang
LD          := ld.lld
PKGTOOL     := $(TOOLCHAIN)/bin/linux/PkgTool.Core

_unused     := $(shell mkdir -p $(INTDIR))

all: $(CONTENT_ID).pkg

$(CONTENT_ID).pkg: pkg.gp4
	$(PKGTOOL) pkg_build $< .

# sce_module/: OpenOrbis libc runtime modules - the app does not start without them.
LIBMODULES  := sce_module/libc.prx sce_module/libSceFios2.prx

pkg.gp4: eboot.bin sce_sys/about/right.sprx sce_sys/param.sfo sce_sys/icon0.png $(LIBMODULES) $(ASSETS)
	$(TOOLCHAIN)/bin/linux/create-gp4 -out $@ --content-id=$(CONTENT_ID) --files "$^"
	# create-gp4 only declares assets/{audio,fonts,images,misc,videos}; PkgTool needs assets/linux too.
	sed -i 's|<dir targ_name="misc" />|<dir targ_name="misc" />\n\t\t\t<dir targ_name="linux" />|' $@
	grep -q '<dir targ_name="linux" />' $@

sce_sys/param.sfo:
	$(PKGTOOL) sfo_new $@
	$(PKGTOOL) sfo_setentry $@ APP_TYPE --type Integer --maxsize 4 --value 1
	$(PKGTOOL) sfo_setentry $@ APP_VER --type Utf8 --maxsize 8 --value '$(VERSION)'
	$(PKGTOOL) sfo_setentry $@ ATTRIBUTE --type Integer --maxsize 4 --value 2  # 0x2: dialogs confirm with Cross
	$(PKGTOOL) sfo_setentry $@ CATEGORY --type Utf8 --maxsize 4 --value 'gd'
	$(PKGTOOL) sfo_setentry $@ CONTENT_ID --type Utf8 --maxsize 48 --value '$(CONTENT_ID)'
	$(PKGTOOL) sfo_setentry $@ DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
	$(PKGTOOL) sfo_setentry $@ SYSTEM_VER --type Integer --maxsize 4 --value 0
	$(PKGTOOL) sfo_setentry $@ TITLE --type Utf8 --maxsize 128 --value '$(TITLE)'
	$(PKGTOOL) sfo_setentry $@ TITLE_ID --type Utf8 --maxsize 12 --value '$(TITLE_ID)'
	$(PKGTOOL) sfo_setentry $@ VERSION --type Utf8 --maxsize 8 --value '$(VERSION)'

eboot.bin: $(OBJS)
	$(LD) $(OBJS) -o $(INTDIR)/$(PROJDIR).elf $(LDFLAGS)
	$(TOOLCHAIN)/bin/linux/create-fself -in=$(INTDIR)/$(PROJDIR).elf -out=$(INTDIR)/$(PROJDIR).oelf --eboot "eboot.bin" --paid 0x3800000000000011

$(INTDIR)/%.o: $(PROJDIR)/%.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f *.pkg pkg.gp4 sce_sys/param.sfo eboot.bin $(INTDIR)/*.elf $(INTDIR)/*.oelf $(OBJS)

.PHONY: all clean
