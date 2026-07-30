CXX ?= g++
PREFIX ?= /usr/local

# Linking libstdc++/libgcc statically is worth roughly 30% of startup: a shell
# is spawned constantly ($SHELL -c per git alias, per system() call), and not
# mapping and relocating a 2MB shared library each time shows up immediately.
# It costs binary size and gives back resident memory. Both flags are probed,
# so a toolchain without them still builds; `make STATIC_CXX= LTO=` opts out
# for a ~180K binary instead of ~390K.
probe = $(shell echo 'int main(){}' | $(CXX) -x c++ $(1) -o /dev/null - 2>/dev/null && echo '$(1)')
STATIC_CXX ?= $(call probe,-static-libstdc++ -static-libgcc)
LTO ?= $(or $(call probe,-flto=auto),$(call probe,-flto))

CXXFLAGS = -std=c++17 -O2 $(LTO) -Wall -Wextra -MMD -MP
LDFLAGS += $(LTO) $(STATIC_CXX)

LUA_PKG := $(shell for p in lua5.4 lua-5.4 lua54 lua; do pkg-config --exists $$p 2>/dev/null && echo $$p && break; done)
ifeq ($(LUA_PKG),)
$(error lua5.4 not found via pkg-config; tried lua5.4, lua-5.4, lua54, lua -- install your distro's lua5.4 dev package)
endif
LUA_CFLAGS := $(shell pkg-config --cflags $(LUA_PKG))
LUA_LIBS := $(shell pkg-config --libs $(LUA_PKG))

CORE_OBJS = lexer.o parser.o executor.o lua_env.o linenoise.o
TESTS = test_lexer test_parser test_lua_env test_executor

wisp: main.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ main.o $(CORE_OBJS) $(LUA_LIBS) -lpthread

# Header dependencies come from -MMD; no hand-maintained lists to drift.
%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(LUA_CFLAGS) -c $<

linenoise.o: linenoise.c
	$(CC) -O2 -Wall -MMD -MP -c $<

# Objects spelled out rather than $^: the generated .d files add headers as
# prerequisites, and those must not reach the link line.
test_lexer: test_lexer.cpp lexer.o
	$(CXX) $(CXXFLAGS) -o $@ test_lexer.cpp lexer.o

test_parser: test_parser.cpp parser.o lexer.o
	$(CXX) $(CXXFLAGS) -o $@ test_parser.cpp parser.o lexer.o

test_lua_env: test_lua_env.cpp lua_env.o
	$(CXX) $(CXXFLAGS) $(LUA_CFLAGS) -o $@ test_lua_env.cpp lua_env.o $(LUA_LIBS)

test_executor: test_executor.cpp executor.o lua_env.o parser.o lexer.o
	$(CXX) $(CXXFLAGS) $(LUA_CFLAGS) -o $@ test_executor.cpp $(CORE_OBJS:linenoise.o=) $(LUA_LIBS) -lpthread

test: $(TESTS) wisp
	@for t in $(TESTS); do ./$$t || exit 1; done
	@sh test_shell.sh
	@command -v python3 >/dev/null && python3 test_interactive.py || \
		echo "test_interactive: skipped (no python3)"

install: wisp
	install -Dm755 wisp $(DESTDIR)$(PREFIX)/bin/wisp
	strip $(DESTDIR)$(PREFIX)/bin/wisp
	@echo "installed $(DESTDIR)$(PREFIX)/bin/wisp -- add it to /etc/shells to use as a login shell"

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/wisp

clean:
	rm -f *.o *.d wisp $(TESTS)

-include $(wildcard *.d)

.PHONY: test install uninstall clean
