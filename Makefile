# make            release 版（LOG_DEBUG 不編入）
# make debug      debug 版（-DDEBUG，執行時再用 -d 開啟）
# make demo       執行 demo.sh
CC      = gcc
CFLAGS  ?= -O2
CFLAGS  += -std=gnu11 -Wall -Wextra -Iinclude -MMD -MP

ifeq ($(DEBUG),1)
CFLAGS  += -g -DDEBUG
endif

LIB      := lib/libnetproto.so
LIB_OBJS := obj/proto.o obj/log.o
BINS     := server client
RPATH    := -Wl,-rpath,'$$ORIGIN/lib' -Wl,--disable-new-dtags

.PHONY: all debug clean demo FORCE

all: $(BINS)

debug:
	$(MAKE) DEBUG=1

demo:
	./demo.sh

$(BINS): %: obj/%.o $(LIB)
	$(CC) $(LDFLAGS) -o $@ $< -Llib -lnetproto $(RPATH)

$(LIB): $(LIB_OBJS) | lib
	$(CC) $(LDFLAGS) -shared -Wl,-soname,libnetproto.so -o $@ $^

$(LIB_OBJS): CFLAGS += -fPIC

obj/%.o: src/%.c obj/.flags | obj
	$(CC) $(CFLAGS) -c -o $@ $<

# 編譯參數改變（例如切換 DEBUG）時強制重新編譯
obj/.flags: FORCE | obj
	@echo '$(CFLAGS)' | cmp -s - $@ 2>/dev/null || echo '$(CFLAGS)' > $@

obj lib:
	mkdir -p $@

clean:
	rm -rf obj lib $(BINS) demo_out

-include $(wildcard obj/*.d)
