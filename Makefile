CC     = gcc
CFLAGS = -std=c17 -O2 -Wall -Wextra -Wno-unused-parameter
LIBS   = $(shell sdl2-config --cflags --libs) -lm

EMCC     = emcc
EMFLAGS  = -std=c17 -O2 -s USE_SDL=2 -s ALLOW_MEMORY_GROWTH=1

SRC      = housekeeper.c
TARGET   = housekeeper
WASM_OUT = index.html

.PHONY: all wasm run serve clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)
	@echo "Native build OK: ./$(TARGET)"

run: $(TARGET)
	./$(TARGET)

wasm: $(SRC) shell.html
	$(EMCC) $(EMFLAGS) --shell-file shell.html -o $(WASM_OUT) $<
	@echo "WASM build OK. Serve with: make serve"

serve:
	@echo "Serving on http://localhost:8000  (Ctrl-C to stop)"
	python3 -m http.server 8000

clean:
	rm -f $(TARGET) index.html index.js index.wasm
