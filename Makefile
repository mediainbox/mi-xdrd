CC = gcc
OPENSSL_PREFIX ?= $(shell brew --prefix openssl 2>/dev/null || echo /usr)
CFLAGS = -Wall -pedantic -std=c99 -c -O2 -I$(OPENSSL_PREFIX)/include
LIBS = -lpthread -L$(OPENSSL_PREFIX)/lib -lcrypto
LIBS_WIN = $(LIBS) -lws2_32
INSTALL = install -c
TARGET = xdrd

PREFIX = $(DESTDIR)/usr
BINDIR = $(PREFIX)/bin

xdrd:	xdrd.o
	$(CC) -o $(TARGET) xdrd.o $(LIBS)

.PHONY:	windows
windows:	xdrd.o
	$(CC) -o $(TARGET) xdrd.o $(LIBS_WIN)

xdrd.o: xdrd.c xdr-protocol.h
	$(CC) $(CFLAGS) xdrd.c

# Build xdrd without main so the WS functions can be linked into the test binary
xdrd_lib.o: xdrd.c xdr-protocol.h
	$(CC) $(CFLAGS) -DXDRD_NO_MAIN -o xdrd_lib.o xdrd.c

test_ws: test_ws.c xdrd_lib.o
	$(CC) -Wall -std=c99 -I$(OPENSSL_PREFIX)/include test_ws.c xdrd_lib.o $(LIBS) -o test_ws

.PHONY: test
test: test_ws
	./test_ws

.PHONY:	clean
clean:
	rm -f *.o xdrd test_ws

.PHONY:	install
install:	xdrd
	$(INSTALL) $(TARGET) $(BINDIR)/$(TARGET)

.PHONY:	uninstall
uninstall:
	rm -f $(BINDIR)/$(TARGET)
