PREFIX ?= /usr

all: samsungipcd

samsungipcd: server.c ppp_crc.c samsung-ipc.c
	gcc -DPREFIX='"$(PREFIX)"' $^ $(shell pkg-config --cflags --libs samsung-ipc) -o $@

samsungipcd-sim: server.c ppp_crc.c phonesim.c
	gcc -DPREFIX='"$(PREFIX)"' $^ -o $@

clean:
	rm -f samsungipcd samsungipcd-sim

install:
	install -Dm0755 samsungipcd $(PREFIX)/sbin/samsungipcd

uninstall:
	rm -f $(PREFIX)/sbin/samsungipcd
