
FLAGS=`pkg-config --cflags --libs libdrm`
FLAGS+=-Wall -O0 -g -lncurses
FLAGS+=-D_FILE_OFFSET_BITS=64

all:
	gcc -o modeset dis_app.c $(FLAGS)
