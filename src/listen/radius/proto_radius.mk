TARGETNAME	:= proto_radius

ifneq "$(TARGETNAME)" ""
TARGET		:= $(TARGETNAME).a
endif

SOURCES		:= proto_radius.c

TGT_PREREQS	:= $(LIBFREERADIUS_SERVER) libfreeradius-radius.a libfreeradius-io.a
