TARGET1 = devcount
TARGET2 = sanlk_load
TARGET3 = sanlk_client
TARGET4 = killpath

SOURCE1 = devcount.c
SOURCE2 = sanlk_load.c
SOURCE3 = sanlk_client.c
SOURCE4 = killpath.c

CFLAGS += -D_GNU_SOURCE -g \
	-Wall \
	-Wformat \
	-Wformat-security \
	-Wnested-externs \
	-Wpointer-arith \
	-Wextra -Wshadow \
	-Wcast-align \
	-Wwrite-strings \
	-Waggregate-return \
	-Wstrict-prototypes \
	-Winline \
	-Wredundant-decls \
	-Wno-sign-compare \
	-Wp,-D_FORTIFY_SOURCE=2 \
	-fexceptions \
	-fasynchronous-unwind-tables \
	-fdiagnostics-show-option

LDFLAGS = -lrt -laio -lblkid -lsanlock

all: $(TARGET1) $(TARGET2) $(TARGET3) $(TARGET4)

$(TARGET1): $(SOURCE1)
	$(CC) $(CFLAGS) $(LDFLAGS) $(SOURCE1) -o $@ -L. -L../src

$(TARGET2): $(SOURCE2)
	$(CC) $(CFLAGS) $(LDFLAGS) $(SOURCE2) -o $@ -L. -L../src

$(TARGET3): $(SOURCE3)
	$(CC) $(CFLAGS) $(LDFLAGS) $(SOURCE3) -o $@ -L. -L../src

$(TARGET4): $(SOURCE4)
	$(CC) $(CFLAGS) $(LDFLAGS) $(SOURCE4) -o $@ -L. -L../src

clean:
	rm -f *.o *.so *.so.* $(TARGET) $(TARGET2) $(TARGET3) $(TARGET4)

