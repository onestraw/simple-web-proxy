CC=gcc
CFLAGS=-Os -W -Wall -Wpointer-arith -Wno-unused-parameter -Werror
SRC=src
DEPS=$(wildcard $(SRC)/*.h)
BINDIR=objs
OBJDIR=$(BINDIR)/$(SRC)
TARGET=$(BINDIR)/webproxy
OBJS=$(patsubst %.c,$(BINDIR)/%.o,$(wildcard $(SRC)/*.c))
VPATH=$(SRC)

all: $(OBJDIR) $(TARGET) 

debug: CFLAGS+= -O0 -g
debug: $(OBJDIR) $(TARGET)

$(TARGET): $(OBJS)
	gcc -o $@ $^ $(CFLAGS)

$(OBJDIR)/%.o: %.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)

$(OBJDIR):
	mkdir -p $(OBJDIR)

clean:
	rm -rf $(BINDIR)
