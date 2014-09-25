CFLAGS=-Wall
mping: mping.o
	${CC} -o $@ mping.o -lcurses



install: mping
	install -m 4755 mping /usr/local/sbin/mping
