CFLAGS = -D_FILE_OFFSET_BITS=64

TARGET = test3proxy
OFILES = test3proxy.o

$(TARGET): $(OFILES)

clean:
	rm -f *.o

test: $(TARGET)
	./test3proxy server & sleep 1; ./test3proxy client && wait
