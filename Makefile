.PHONY: all setup build run clean
all: build

setup:
	./scripts/setup.sh

build:
	./scripts/build.sh

run:
	./scripts/run.sh

clean:
	./scripts/clean.sh
