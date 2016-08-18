.PHONY: clean-tests
clean-tests:
	rm -rf tests
	rm -f config.h

.PHONY: clean
clean: clean-tests

config.h: $(addprefix tests/,$(addsuffix .h,$(TESTS)))
	cat tests/*.h > config.h

tests/%.h: ../build-aux/tests/%.c
	@if [ ! -d tests ]; then mkdir -p tests; fi
	@ln -sf ../../build-aux/tests/$*.c tests/$*.c
	@if $(CC) $(CFLAGS) $(CPPFLAGS) -Werror=incompatible-pointer-types -c tests/$*.c -o /dev/null 2>tests/$*.log; then \
	   echo "# tests/$*: Yes" && tail -n 1 $< > $@; \
	 else \
	   echo "# tests/$*: No" && true > $@; \
	 fi

-include ../build-aux/tests/*.d
