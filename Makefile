#---------------------------------------------------------------------------------
# Clear the implicit built in rules
#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------
ifeq ($(strip $(PSL1GHT)),)
$(error "Please set PSL1GHT in your environment. export PSL1GHT=<path>")
endif

include $(PSL1GHT)/ppu_rules

Q		=	$(if $(filter 1,$(VERBOSE)),, @)

#---------------------------------------------------------------------------------
# ManaGunZ : "make pkg"
# FileManager : "FILEMANAGER=1 make pkg"
#---------------------------------------------------------------------------------

#---------------------------------------------------------------------------------
# TARGET is the name of the output
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# INCLUDES is a list of directories containing extra header files
#---------------------------------------------------------------------------------

BUILD		:=	build
SOURCES		:=	source
DATA		:=	data
INCLUDES	:=	include
PKGFILES	:=	$(CURDIR)/pkgfiles
SFOXML		:=	sfo.xml

VERSION		:=	1.42

ifeq ($(BDVD), 1)
MACHDEP		+= -D_BDVD_BUILD_
endif

PKGFILES	:=	$(PKGFILES)
TARGET		:=	ManaGunZ
TITLE		:=	$(TARGET) v$(VERSION) $(TAIL)
APPID		:=	MANAGUNZ0
CONTENTID	:=	EP0001-$(APPID)_00-0000000000000000

SCETOOL_FLAGS	+=	--self-ctrl-flags 4000000000000000000000000000000000000000000000000000000000000002
SCETOOL_FLAGS	+=	--self-cap-flags 00000000000000000000000000000000000000000000007B0000000100000000

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------

CFLAGS		=	-O2 -Wall -mcpu=cell $(MACHDEP) $(INCLUDE)
CXXFLAGS	=	$(CFLAGS)

LDFLAGS		=	$(MACHDEP) -Wl,-Map,$(notdir $@).map

#---------------------------------------------------------------------------------
# any extra libraries we wish to link with the project
#---------------------------------------------------------------------------------
LIBS	:= 

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
LIBDIRS	:=	$(PORTLIBS)

#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)_v$(VERSION)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
					$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

export BUILDDIR	:=	$(CURDIR)/$(BUILD)

#---------------------------------------------------------------------------------
# automatically build a list of object files for our project
#---------------------------------------------------------------------------------
CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
sFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.S)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.bin)))
PNGFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.png)))
JPGFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.jpg)))
TTFFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.ttf)))
VCGFILES	:=	$(foreach dir,$(SHADERS),$(notdir $(wildcard $(dir)/*.vcg)))
FCGFILES	:=	$(foreach dir,$(SHADERS),$(notdir $(wildcard $(dir)/*.fcg)))

#---------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
	export LD	:=	$(CC)
else
	export LD	:=	$(CXX)
endif

export OFILES	:=	$(addsuffix .o,$(BINFILES)) \
			$(addsuffix .o,$(TTFFILES)) \
			$(addsuffix .o,$(VPOFILES)) \
			$(addsuffix .o,$(FPOFILES)) \
			$(addsuffix .o,$(PNGFILES)) \
			$(addsuffix .o,$(JPGFILES)) \
					$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) \
					$(sFILES:.s=.o) $(SFILES:.S=.o)

#---------------------------------------------------------------------------------
# build a list of include paths
#---------------------------------------------------------------------------------
export INCLUDE	:=	-I$(PORTLIBS)/include/freetype2 \
			$(foreach dir,$(INCLUDES), -I$(CURDIR)/$(dir)) \
					$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
					$(LIBPSL1GHT_INC) \
					-I$(CURDIR)/$(BUILD)

#---------------------------------------------------------------------------------
# build a list of library paths
#---------------------------------------------------------------------------------
export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib) \
					$(LIBPSL1GHT_LIB)

export OUTPUT	:=	$(CURDIR)/$(TARGET)_v$(VERSION)
.PHONY: $(BUILD) clean

#---------------------------------------------------------------------------------
$(BUILD):
	$(Q)$(MAKE) small_clean --no-print-directory
	$(Q)$(MAKE) -C MGZ --no-print-directory
	$(Q)cp -f MGZ/MGZ.self $(PKGFILES)/USRDIR/$(TARGET).self
	$(Q)[ -d $@ ] || mkdir -p $@
	$(Q)$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile
ifeq ($(FILEMANAGER), 1)
	$(Q)cp -fr $(PKGFILES1)/USRDIR/GUI/common $(PKGFILES2)/USRDIR/GUI
	$(Q)cp -fr $(PKGFILES1)/USRDIR/GUI/colorset.ini $(PKGFILES2)/USRDIR/GUI
	$(Q)cp -fr $(PKGFILES1)/USRDIR/sys/data $(PKGFILES2)/USRDIR/sys
	$(Q)cp -fr $(PKGFILES1)/USRDIR/sys/loc $(PKGFILES2)/USRDIR/sys
	$(Q)cp -fr $(PKGFILES1)/USRDIR/sys/RCO $(PKGFILES2)/USRDIR/sys
	$(Q)cp -fr $(PKGFILES1)/USRDIR/sys/Check.zip $(PKGFILES2)/USRDIR/sys
	$(Q)cp -fr $(PKGFILES1)/USRDIR/sys/sprx_iso $(PKGFILES2)/USRDIR/sys
	$(Q)cp -fr $(PKGFILES1)/USRDIR/sys/PSP_CRC.txt $(PKGFILES2)/USRDIR/sys
	$(Q)cp -fr $(PKGFILES1)/USRDIR/sys/dev_klics.txt $(PKGFILES2)/USRDIR/sys
endif
	
#---------------------------------------------------------------------------------
# To compile filemanager and managunz without recompiling everything,
# it'll recompile files where the "FILEMANAGER" and "RPCS3" are used
#---------------------------------------------------------------------------------

small_clean:
	$(Q)rm -fr $(BUILD) *.elf *.self $(TARGET)_v*.pkg
	$(Q)rm -fr MGZ/build/main.o

#---------------------------------------------------------------------------------
clean:
	$(Q)echo clean ...
	$(Q)rm -fr $(BUILD) *.elf *.self *.pkg
	$(Q)$(MAKE) clean -C MGZ --no-print-directory
	$(Q)$(MAKE) clean -C MGZ/lib/cobra --no-print-directory
	$(Q)$(MAKE) clean -C MGZ/lib/ImageMagick --no-print-directory
	$(Q)$(MAKE) clean -C MGZ/lib/libapputil --no-print-directory
	$(Q)$(MAKE) clean -C MGZ/lib/libgtfconv --no-print-directory
	$(Q)$(MAKE) clean -C MGZ/lib/libiconv --no-print-directory
	$(Q)$(MAKE) clean -C MGZ/lib/libntfs_ext --no-print-directory
	$(Q)$(MAKE) clean -C payloads/MAMBA --no-print-directory
	$(Q)$(MAKE) clean -C payloads/PS2_EMU --no-print-directory
	$(Q)$(MAKE) clean -C payloads/rawseciso --no-print-directory
	$(Q)rm -fr $(PKGFILES1)/USRDIR/$(TARGET).self
	$(Q)rm -fr $(PKGFILES2)/USRDIR
	
#---------------------------------------------------------------------------------
update:
	cd OffsetFinder; ./OffsetFinder.exe extract
	cd OffsetFinder; ./OffsetFinder.exe search
	cd OffsetFinder; ./OffsetFinder.exe move
	$(Q)$(MAKE) -C payloads/SKY --no-print-directory
	$(Q)rm -f payloads/MAMBA/bin/*.bin
	$(Q)rm -f payloads/MAMBA/bin/debug/*.bin
	$(Q)$(MAKE) release -C payloads/MAMBA --no-print-directory
	$(Q)$(MAKE) loader -C payloads/MAMBA --no-print-directory
	$(Q)mv -f payloads/MAMBA/bin/mamba_*.lz.bin  MGZ/data
	$(Q)mv -f payloads/MAMBA/bin/mamba_loader_*.bin  MGZ/data
	$(Q)$(MAKE) all -C payloads/PS2_EMU --no-print-directory
	$(Q)mv -f payloads/PS2_EMU/BIN/*.bin  MGZ/data
	$(Q)$(MAKE) -C payloads/rawseciso --no-print-directory
	$(Q)mv -f payloads/rawseciso/rawseciso.sprx $(PKGFILES)/USRDIR/sys/sprx_iso
	$(Q)$(MAKE) -C payloads/erk_dumper/spu --no-print-directory
	$(Q)$(MAKE) -C payloads/erk_dumper/source --no-print-directory
	$(Q)mv -f payloads/erk_dumper/bin/*.bin  MGZ/data
#---------------------------------------------------------------------------------
translate:
	cd TranslateMGZ; ./TranslateMGZ.exe ../MGZ/source/str.h ../pkgfiles/USRDIR/sys/loc/
	
#---------------------------------------------------------------------------------
lib:
	$(MAKE) -C MGZ/lib/cobra --no-print-directory
	$(Q)mv -f MGZ/lib/cobra/libcobra.a MGZ/lib/libcobra.a
	$(MAKE) -C MGZ/lib/libntfs_ext --no-print-directory
	$(MAKE) -C MGZ/lib/OpenSSL --no-print-directory
	$(Q)mv -f MGZ/lib/OpenSSL/libcrypto.a MGZ/lib/libcrypto.a
	$(Q)mv -f MGZ/lib/OpenSSL/libssl.a MGZ/lib/libssl.a
#---------------------------------------------------------------------------------
ntfs:
	$(MAKE) -C MGZ/lib/libntfs_ext --no-print-directory
#---------------------------------------------------------------------------------
run:
	ps3load $(OUTPUT).self

#---------------------------------------------------------------------------------
pkg: $(BUILD) $(OUTPUT).pkg

#---------------------------------------------------------------------------------
else

DEPENDS	:=	$(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
$(OUTPUT).self: $(OUTPUT).elf
$(OUTPUT).elf:	$(OFILES)

#---------------------------------------------------------------------------------
# This rule links in binary data with the .bin extension
#---------------------------------------------------------------------------------
%.bin.o	:	%.bin
#---------------------------------------------------------------------------------
	$(Q)echo $(notdir $<)
	$(Q)$(bin2o)

#---------------------------------------------------------------------------------
%.ttf.o	:	%.ttf
#---------------------------------------------------------------------------------
	$(Q)echo $(notdir $<)
	$(Q)$(bin2o)

#---------------------------------------------------------------------------------
%.vpo.o	:	%.vpo
#---------------------------------------------------------------------------------
	$(Q)echo $(notdir $<)
	$(Q)$(bin2o)

#---------------------------------------------------------------------------------
%.fpo.o	:	%.fpo
#---------------------------------------------------------------------------------
	$(Q)echo $(notdir $<)
	$(Q)$(bin2o)

#---------------------------------------------------------------------------------
%.jpg.o	:	%.jpg
#---------------------------------------------------------------------------------
	$(Q)echo $(notdir $<)
	$(Q)$(bin2o)
#---------------------------------------------------------------------------------
%.png.o	:	%.png
#---------------------------------------------------------------------------------
	$(Q)echo $(notdir $<)
	$(Q)$(bin2o)

-include $(DEPENDS)

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
