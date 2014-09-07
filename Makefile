mping: mping.o
	${CC} -o $@ -lcurses



install: mping
	install -m 4755 mping /usr/local/sbin/mping
