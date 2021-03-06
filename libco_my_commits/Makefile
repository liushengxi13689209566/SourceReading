#
# Tencent is pleased to support the open source community by making Libco available.
# 
# Copyright (C) 2014 THL A29 Limited, a Tencent company. All rights reserved.
# 
# Licensed under the Apache License, Version 2.0 (the "License"); 
# you may not use this file except in compliance with the License. 
# You may obtain a copy of the License at
# 
#   http://www.apache.org/licenses/LICENSE-2.0
# 
# Unless required by applicable law or agreed to in writing, 
# software distributed under the License is distributed on an "AS IS" BASIS, 
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. 
# See the License for the specific language governing permissions and 
# limitations under the License.
#


##### Makefile Rules ##########
MAIL_ROOT=.
SRCROOT=.

##define the compliers
CPP = $(CXX)
AR = ar -rc
RANLIB = ranlib

CPPSHARE = $(CPP) -fPIC -shared -O2 -pipe -L$(SRCROOT)/solib/ -o 
CSHARE = $(CC) -fPIC -shared -O2 -pipe -L$(SRCROOT)/solib/ -o 

#编译参数　
CFLAGS= -g $(INCLS) -fPIC -DLINUX -pipe -c -fno-inline

#静态库地址
STATICLIBPATH=$(SRCROOT)/lib
#动态库地址
DYNAMICLIBPATH=$(SRCROOT)/solib

INCLS += -I$(SRCROOT)

## default links
ifeq ($(LINKS_DYNAMIC), 1)
LINKS += -L$(DYNAMICLIBPATH) -L$(STATICLIBPATH)
else
LINKS += -L$(STATICLIBPATH)
endif

CPPSRCS  = $(wildcard *.cpp)
CSRCS  = $(wildcard *.c)

CPPOBJS  = $(patsubst %.cpp,%.o,$(CPPSRCS))
COBJS  = $(patsubst %.c,%.o,$(CSRCS))

SRCS = $(CPPSRCS) $(CSRCS)
OBJS = $(CPPOBJS) $(COBJS)

CPPCOMPI=$(CPP) $(CFLAGS) -Wno-deprecated
CCCOMPI=$(CC) $(CFLAGS)

#生成可执行文件
BUILDEXE = $(CPP) $(BFLAGS) -o $@ $^ $(LINKS) 

CLEAN = rm -f *.o 

#生成cpp.o文件
CPPCOMPILE = $(CPPCOMPI) $< $(FLAGS) $(INCLS) $(MTOOL_INCL) -o $@
#生成c.o文件
CCCOMPILE = $(CCCOMPI) $< $(FLAGS) $(INCLS) $(MTOOL_INCL) -o $@

#生成静态库
ARSTATICLIB = $(AR) $@.tmp $^ $(AR_FLAGS); \
			  if [ $$? -ne 0 ]; then exit 1; fi; \
			  test -d $(STATICLIBPATH) || mkdir -p $(STATICLIBPATH); \
			  mv -f $@.tmp $(STATICLIBPATH)/$@;
			
#生成动态库
BUILDSHARELIB = $(CPPSHARE) $@.tmp $^ $(BS_FLAGS); \
				if [ $$? -ne 0 ]; then exit 1; fi; \
				test -d $(DYNAMICLIBPATH) || mkdir -p $(DYNAMICLIBPATH); \
				mv -f $@.tmp $(DYNAMICLIBPATH)/$@;

.cpp.o:
	$(CPPCOMPILE)
.c.o:
	$(CCCOMPILE)


########## options ##########
#编译参数
CFLAGS += -g -fno-strict-aliasing -O2 -Wall -export-dynamic \
	-Wall -pipe  -D_GNU_SOURCE -D_REENTRANT -fPIC -Wno-deprecated -m64

#链接的库
LINKS += -g -L./lib -lcolib -lpthread -ldl

#生成静态库和动态库时需要的文件
COLIB_OBJS=co_epoll.o co_routine.o co_hook_sys_call.o coctx_swap.o coctx.o

PROGS = colib liu_hello liu_EchoServer 
# example_poll example_echosvr example_echocli example_thread  example_cond example_specific example_copystack example_closure 

#make 时默认编译 PROGS
all:$(PROGS)

#两个库
colib:libcolib.a libcolib.so

libcolib.a: $(COLIB_OBJS)
	$(ARSTATICLIB) 
libcolib.so: $(COLIB_OBJS)
	$(BUILDSHARELIB) 

# example_echosvr:example_echosvr.o
# 	$(BUILDEXE) 
# example_echocli:example_echocli.o
# 	$(BUILDEXE) 
example_thread:example_thread.o
	$(BUILDEXE) 
# example_poll:example_poll.o
# 	$(BUILDEXE) 
# example_exit:example_exit.o
# 	$(BUILDEXE) 
# example_cond:example_cond.o
# 	$(BUILDEXE)
# example_specific:example_specific.o
# 	$(BUILDEXE)
# example_copystack:example_copystack.o
# 	$(BUILDEXE)
# example_setenv:example_setenv.o
# 	$(BUILDEXE)
# example_closure:example_closure.o
# 	$(BUILDEXE)
liu_hello:liu_hello.o
	$(BUILDEXE)
liu_EchoServer:liu_EchoServer.o
	$(BUILDEXE)

dist: clean libco-$(version).src.tar.gz

clean:
	$(CLEAN) *.o $(PROGS)
	rm -fr MANIFEST lib solib libco-$(version).src.tar.gz libco-$(version)

