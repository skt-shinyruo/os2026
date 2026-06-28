SUBDIR := mymalloc

.PHONY: all check test clean

all:
	$(MAKE) -C $(SUBDIR) all

check:
	$(MAKE) -C $(SUBDIR) check

test:
	$(MAKE) -C $(SUBDIR) test

clean:
	$(MAKE) -C $(SUBDIR) clean
