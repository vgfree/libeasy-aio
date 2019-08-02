
all:
	gcc -g -Wall -std=gnu99 test.c eaio_api.c etask.c eaio_logger.c -lpthread -laio -o eaio

clean:
	rm -f ./eaio
