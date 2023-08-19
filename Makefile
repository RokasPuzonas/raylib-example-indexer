.DEFAULT_GOAL := run
.PHONY := raylib_example_indexer all clean

raylib_example_indexer: src/main.c
	mkdir -p output
	gcc src/main.c -o output/raylib_example_indexer -I./external -DSTB_C_LEXER_IMPLEMENTATION

raylib_api:
	mkdir -p output
	curl https://raw.githubusercontent.com/raysan5/raylib/master/parser/output/raylib_api.txt -o output/raylib_api.txt

raylib:
	mkdir -p output
	git clone git@github.com:raysan5/raylib.git output/raylib

run: raylib_example_indexer
	./output/raylib_example_indexer output/raylib_api.txt output/raylib/examples output/output.json
	./output/raylib_example_indexer output/raylib_api.txt output/raylib/examples output/output.csv

all: raylib_api raylib run

clean:
	rm -rf output
