.PHONY: all setup build run coop clean test
all: build

# Local co-op test: 1 host + (N-1) clients on loopback. Override count: make coop N=3
N ?= 2

setup:
	./scripts/setup.sh

build:
	./scripts/build.sh

run:
	./scripts/run.sh

coop:
	./scripts/coop.sh $(N)

clean:
	./scripts/clean.sh

test:
	./scripts/test.sh
