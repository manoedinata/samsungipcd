PREFIX ?= /usr

all: samsungipcd ppp-hook.so

ppp_crc.o: ppp_crc.c
	gcc ppp_crc.c -O3 -c -o ppp_crc.o

samsungipcd: server.c ppp_crc.o samsung-ipc.c modem.h buffers.h
	gcc -DPREFIX='"$(PREFIX)"' server.c ppp_crc.o samsung-ipc.c $(shell pkg-config --cflags --libs samsung-ipc) -o $@

samsungipcd-sim: server.c ppp_crc.o phonesim.c modem.h buffers.h
	gcc -DPREFIX='"$(PREFIX)"' server.c ppp_crc.o phonesim.c -o $@

ppp-hook.so: ppp_hook.c ppp_crc.o
	gcc -DPREFIX='"$(PREFIX)"' ppp_hook.c ppp_crc.o -fPIE -fPIC -shared -pthread -o ppp-hook.so -O0 -g -DPPP_DUMP

clean:
	rm -f samsungipcd samsungipcd-sim ppp_crc.o

install:
	install -Dm0755 samsungipcd $(PREFIX)/sbin/samsungipcd
	install -Dm0755 ppp-hook.so $(PREFIX)/share/samsungipcd/ppp-hook.so

uninstall:
	rm -f $(PREFIX)/sbin/samsungipcd
