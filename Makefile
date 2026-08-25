CC     ?= gcc
CFLAGS ?= -O2 -fPIC -shared -Wall -Wextra
PREFIX ?= /usr/local

all: xifevent-shim.so

xifevent-shim.so: src/xifevent-shim.c
	$(CC) $(CFLAGS) -o $@ $< -ldl -lX11

install: all
	install -d $(DESTDIR)$(PREFIX)/lib
	install -m 755 xifevent-shim.so $(DESTDIR)$(PREFIX)/lib/xifevent-shim.so

clean:
	rm -f xifevent-shim.so

.PHONY: all install clean
