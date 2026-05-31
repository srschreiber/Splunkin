.PHONY: all setup build run clean test
all: build

setup:
	./scripts/setup.sh

build:
	./scripts/build.sh

run:
	./scripts/run.sh

clean:
	./scripts/clean.sh

test:
	./scripts/test.sh
