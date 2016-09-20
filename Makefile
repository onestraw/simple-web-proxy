CC=gcc
CFLAGS=-O -W -Wall -Wpointer-arith -Wno-unused-parameter -Werror -g
SRC=src
DEPS=$(wildcard $(SRC)/*.h)
BINDIR=objs
OBJDIR=$(BINDIR)/$(SRC)
TARGET=$(BINDIR)/webproxy
OBJS=$(patsubst %.c,$(BINDIR)/%.o,$(wildcard $(SRC)/*.c))
VPATH=$(SRC)

all: $(OBJDIR) $(TARGET) 

$(TARGET): $(OBJS)
	gcc -o $@ $^ $(CFLAGS)

$(OBJDIR)/%.o: %.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)

$(OBJDIR):
	mkdir -p $(OBJDIR)

clean:
	rm -rf $(BINDIR)
