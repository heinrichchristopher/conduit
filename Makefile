# Conduit - FTP server for the original Xbox (nxdk)
#
#   PATH=$PWD/nxdk/bin:$PATH make        ->  bin/default.xbe  and  Conduit.iso
#
# nxdk is expected in ./nxdk (git submodule) or ../nxdk; override with NXDK_DIR=...
XBE_TITLE = Conduit
GEN_XISO = $(XBE_TITLE).iso
SRCS = $(addprefix $(CURDIR)/src/, main.c ftpd.c tunnel.c dashassets.c gen_assets.c)
NXDK_DIR ?= $(firstword $(wildcard $(CURDIR)/nxdk) $(abspath $(CURDIR)/../nxdk))
export NXDK_DIR
NXDK_NET = y
NXDK_SDL = y

# RX_BUFF_CNT / TCP_OOSEQ_MAX_PBUFS: the NIC driver has only 64 zero-copy receive buffers by default. With several
# uploads at once, out-of-order segments could hold all of them, so the missing segment could never be received and
# every upload stalled until the receive timeout. 256 buffers (512 KB) plus a per-connection limit avoid that.
#
# SO_REUSE: when the Xbox closes a data connection first (LIST, RETR), its port stays in TIME_WAIT for 2 minutes.
# Without SO_REUSEADDR on the passive socket, the 100 passive ports were used up after 100 such transfers within
# 2 minutes ("425 Kein freier Port"), e.g. when downloading a folder with many small files.
# TCP_MSL: TIME_WAIT lasts 2 * TCP_MSL. lwIP's default (2 minutes) is meant for the internet; on a LAN 4 seconds are
# plenty. With thousands of connections in TIME_WAIT (many small downloads) a new data connection occasionally got
# no answer to its SYN and hung until the client gave up.
#
# lwIP in nxdk allows only 4 sockets in total (MEMP_NUM_NETCONN). An FTP server needs a listener plus
# control, passive and data sockets per client. The setting also has to apply when the lwIP objects are
# built inside the nxdk tree; if it changes, those objects are rebuilt once (stamp: nxdk/lib/net/.netdefs).
#
# Values that nxdk's lwipopts.h sets unconditionally (debug output) are changed in src/lwip_tune.h,
# which is force-included into lwIP and Conduit sources only.
# LWIP_TUNE_REV: bump when src/lwip_tune.h changes, so the lwIP objects are rebuilt.
NET_DEFS = -DMEMP_NUM_NETCONN=40 -DLWIP_DHCP_DOES_ACD_CHECK=0 -DLWIP_SO_SNDTIMEO=1 -DLWIP_SO_RCVTIMEO=1 \
           -DRX_BUFF_CNT=256 -DTCP_OOSEQ_MAX_PBUFS=8 -DSO_REUSE=1 -DTCP_MSL=2000UL -DLWIP_TUNE_REV=2
NET_STAMP := $(NXDK_DIR)/lib/net/.netdefs
ifneq ($(shell cat $(NET_STAMP) 2>/dev/null),$(NET_DEFS))
$(info Rebuilding lwIP objects with $(NET_DEFS))
$(shell rm -f $(NXDK_DIR)/lib/libnxdk_net.lib; find $(NXDK_DIR)/lib/net -name '*.obj' -delete; echo '$(NET_DEFS)' > $(NET_STAMP))
endif
$(NXDK_DIR)/lib/net/%.obj $(CURDIR)/src/%.obj: CFLAGS += -include $(CURDIR)/src/lwip_tune.h

override CFLAGS += -I$(CURDIR)/src -O2 $(NET_DEFS)
# Quellpfade relativ einbauen (assert() & Co. speichern __FILE__): keine lokalen Pfade im Release-XBE,
# und der Build ist unabhaengig davon, wo das Repo liegt. Betrifft auch nxdk; dessen schon gebaute
# Bibliotheken behalten ihre Pfade, bis sie neu gebaut werden.
PREFIX_MAP := -ffile-prefix-map=$(NXDK_DIR)/=nxdk/ -ffile-prefix-map=$(CURDIR)/=
override CFLAGS += $(PREFIX_MAP)
override CXXFLAGS += $(PREFIX_MAP)

include $(NXDK_DIR)/Makefile
