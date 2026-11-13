# make            release build (LOG_DEBUG not compiled in)
# make debug      debug build (-DDEBUG; enable at run time with -d)
# make demo       run demo.sh
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

# Force a rebuild when the compiler flags change (e.g. switching DEBUG)
obj/.flags: FORCE | obj
	@echo '$(CFLAGS)' | cmp -s - $@ 2>/dev/null || echo '$(CFLAGS)' > $@

obj lib:
	mkdir -p $@

clean:
	rm -rf obj lib $(BINS) demo_out

-include $(wildcard obj/*.d)
