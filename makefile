# make to nob shim
.DEFAULT_GOAL := default
$(MAKEFILE_LIST): ; # stop make remaking itself
.PHONY: FORCE default
default: FORCE
	@./nob.c
%: FORCE
	@./nob.c $@
