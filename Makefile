CC=gcc
CFLAGS=-Os -W -Wall -Wpointer-arith -Wno-unused-parameter -Werror
SRC=src
DEPS=$(wildcard $(SRC)/*.h)
BINDIR=objs
OBJDIR=$(BINDIR)/$(SRC)
TARGET=$(BINDIR)/webproxy
TEST_BIN=tests/test_regress
OBJS=$(patsubst %.c,$(BINDIR)/%.o,$(wildcard $(SRC)/*.c))
VPATH=$(SRC)

all: $(OBJDIR) $(TARGET)

debug: CFLAGS+= -O0 -g
debug: $(OBJDIR) $(TARGET)

test: $(TEST_BIN)
	./$(TEST_BIN)

$(TARGET): $(OBJS)
	gcc -o $@ $^ $(CFLAGS)

$(TEST_BIN): tests/test_regress.c
	$(CC) -I. -Wall -Wextra -Wno-unused-parameter -Werror -o $@ $<

$(OBJDIR)/%.o: %.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)

$(OBJDIR):
	mkdir -p $(OBJDIR)

clean:
	rm -rf $(BINDIR) $(TEST_BIN)
