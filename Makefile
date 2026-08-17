.PHONY: all clean pflayer amlayer dblayer test

all: pflayer amlayer dblayer

pflayer:
	$(MAKE) -C pflayer

amlayer: pflayer
	$(MAKE) -C amlayer

dblayer: amlayer
	$(MAKE) -C dblayer

test: all
	bash dblayer/test.sh

clean:
	$(MAKE) -C pflayer clean
	$(MAKE) -C amlayer clean
	$(MAKE) -C dblayer clean
