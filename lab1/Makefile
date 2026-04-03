CXX      ?= g++
CXXFLAGS  = -std=c++17 -g -O0 -Wall

all: repro repro_asan poc.tgs

repro: repro.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<
	@echo "[+] built: $@"

repro_asan: repro.cpp
	$(CXX) $(CXXFLAGS) -fsanitize=address -o $@ $<
	@echo "[+] built: $@"

poc.tgs poc.json: gen_poc.py
	python3 gen_poc.py --out poc.tgs --json poc.json --teeth 512 --width 1024 --height 128
	@echo "[+] generated poc.tgs and poc.json"

run: repro
	./repro

run_asan: repro_asan
	./repro_asan; true

clean:
	rm -f repro repro_asan poc.tgs poc.json

.PHONY: all run run_asan clean
