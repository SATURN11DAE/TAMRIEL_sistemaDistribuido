CC      = gcc
CFLAGS  = -Wall -Wextra -g -pthread -I.
LIBS    = -lmicrohttpd -lcurl -lpthread
SRC     = main.c util/configuracoes.c util/lamport.c util/util.c util/persistencia.c http/httpCliente.c http/httpServidor.c cliServidor/cli.c
OBJ     = $(SRC:.c=.o)

servidor: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) servidor
