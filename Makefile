ADMIN_SOCK = $(RUN_PATH)/admin.sock
BIN_PATH := .
CC := gcc
RUN_PATH := .
SEAMLESS_BIN = $(BIN_PATH)/$(SEAMLESS_PROG)
SEAMLESS_PROG := seamless
SRCS := main.c worker.c

all: build

build:
	@mkdir -p $(BIN_PATH)
	$(CC) -o $(SEAMLESS_BIN) $(SRCS)

clean:
	rm -rf $(ADMIN_SOCK) $(SEAMLESS_BIN)

# Reloads master which triggers handoff. The pkill used in this target is not
# fool-proof. It assumes you have pkill and that you have no other running
# processes whose name is 'seamless'.
reload:
	pkill --oldest --signal SIGUSR2 $(SEAMLESS_PROG)

run: start

scan:
	scan-build-6.0 $(CC) -o $(SEAMLESS_BIN) $(SRCS)

start:
	@mkdir -p $(RUN_PATH)
	@$(SEAMLESS_BIN)

stop:
	@-pkill $(SEAMLESS_PROG)
	@rm -rf $(ADMIN_SOCK)

.PHONY: build clean reload run scan start stop
