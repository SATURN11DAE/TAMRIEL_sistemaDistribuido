CC      = gcc
CFLAGS  = -Wall -Wextra -g -pthread -I. -MMD -MP
LIBS    = -lmicrohttpd -lcurl -lcrypto -lpthread

SRC     = main.c util/configuracoes.c util/lamport.c util/util.c util/persistencia.c util/auth.c util/json.c consorcio/cadastro.c consorcio/busca.c consorcio/transacao.c http/httpCliente.c http/httpServidor.c cliServidor/cli.c
OBJ     = $(SRC:.c=.o)

SRC_CLI = cliUsuario/terminal.c cliUsuario/apiConsorcio.c util/json.c util/util.c util/configuracoes.c
OBJ_CLI = $(SRC_CLI:.c=.o)

all: servidor cliente

servidor: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LIBS)

cliente: $(OBJ_CLI)
	$(CC) $(CFLAGS) -o $@ $(OBJ_CLI) -lcurl

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(OBJ_CLI) $(OBJ:.o=.d) $(OBJ_CLI:.o=.d) servidor cliente

-include $(OBJ:.o=.d) $(OBJ_CLI:.o=.d)

.PHONY: all clean
