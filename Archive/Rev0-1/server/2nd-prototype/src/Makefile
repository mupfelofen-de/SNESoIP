# -*-makefile-*-
.PHONY: all clean


all:
	gcc \
	-lconfig `mysql_config --cflags --libs` \
	-o ../server -lpthread \
	main.c trivium.c tcp.c mysql.c utils.c


clean:
	rm ../server
