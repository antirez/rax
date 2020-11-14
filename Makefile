LIBNAME ?=	librax.so

PREFIX ?=	/usr
LIBDIR ?=	$(PREFIX)/lib
INCLUDEDIR ?=	$(PREFIX)/include

INSTALL ?=	install

CFLAGS +=	-Wall -W -std=c99
LDFLAGS +=	-lm

# Uncomment the following two lines for coverage testing
#
# CFLAGS +=	-fprofile-arcs -ftest-coverage
# LDFLAGS +=	-lgcov

# Uncomment for debug build
# CFLAGS +=	-g -ggdb

all: $(LIBNAME)
test: rax-test rax-oom-test
	LD_LIBRARY_PATH=. ./rax-test --units
	LD_LIBRARY_PATH=. ./rax-oom-test > /dev/null

$(LIBNAME): rax.o
	$(CC) -shared $^ -o $@ -Wl,--soname,$@ $(LDFLAGS)

rax.o: rax.h
rax-test.o: rax.h
rax-oom-test.o: rax.h

rax-test: rax-test.o rc4rand.o crc16.o
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS) -L. -lrax

rax-oom-test: rax-oom-test.o
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS) -L. -lrax

.c.o:
	$(CC) -fPIC -shared -c $(CFLAGS) $<

clean:
	rm -f rax-test rax-oom-test *.gcda *.gcov *.gcno *.o $(LIBNAME)

install: $(LIBNAME)
	$(INSTALL) -m644 rax.h -D $(DESTDIR)$(INCLUDEDIR)/rax.h
	$(INSTALL) -m644 $(LIBNAME) -D $(DESTDIR)$(LIBDIR)/$(LIBNAME)
