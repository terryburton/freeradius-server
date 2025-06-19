TARGETNAME	:= rlm_kafka

ifneq "$(TARGETNAME)" ""
TARGET		:= $(TARGETNAME)$(L)
endif

SOURCES		:= $(TARGETNAME).c

SRC_CFLAGS	:=   
TGT_LDLIBS	:=  -lrdkafka 
LOG_ID_LIB	= 63
