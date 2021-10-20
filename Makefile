
all: eaio_api.o etask.o eaio_logger.o
	@gcc -g -std=gnu99 -Wall test.c eaio_api.c etask.c eaio_logger.c -lpthread -laio -o eaio
	@ar -rcs libeaio.a $^

%.o: %.c
	@gcc -fPIC -g -std=gnu99 -Wall -Wno-unused-function -Wno-unused-variable -c $< -o $@

clean:
	rm -f ./eaio
	rm -f ./*.o
	rm -f ./libeaio.a
