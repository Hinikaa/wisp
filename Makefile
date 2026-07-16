CXX ?= g++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra

LUA_PKG := $(shell for p in lua5.4 lua-5.4 lua54 lua; do pkg-config --exists $$p 2>/dev/null && echo $$p && break; done)
ifeq ($(LUA_PKG),)
$(error lua5.4 not found via pkg-config; tried lua5.4, lua-5.4, lua54, lua -- install your distro's lua5.4 dev package)
endif
LUA_CFLAGS := $(shell pkg-config --cflags $(LUA_PKG))
LUA_LIBS := $(shell pkg-config --libs $(LUA_PKG))

CORE_OBJS = lexer.o parser.o executor.o lua_env.o linenoise.o

wisp: main.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) -o wisp main.o $(CORE_OBJS) $(LUA_LIBS) -lpthread

main.o: main.cpp executor.h lua_env.h linenoise.h
	$(CXX) $(CXXFLAGS) $(LUA_CFLAGS) -c main.cpp

lexer.o: lexer.cpp lexer.h
	$(CXX) $(CXXFLAGS) -c lexer.cpp

parser.o: parser.cpp parser.h lexer.h
	$(CXX) $(CXXFLAGS) -c parser.cpp

executor.o: executor.cpp executor.h lua_env.h parser.h
	$(CXX) $(CXXFLAGS) $(LUA_CFLAGS) -c executor.cpp

lua_env.o: lua_env.cpp lua_env.h builtins.h
	$(CXX) $(CXXFLAGS) $(LUA_CFLAGS) -c lua_env.cpp

linenoise.o: linenoise.c linenoise.h
	$(CC) -O2 -Wall -c linenoise.c

test_lexer: test_lexer.cpp lexer.o
	$(CXX) $(CXXFLAGS) -o test_lexer test_lexer.cpp lexer.o

test_parser: test_parser.cpp parser.o lexer.o
	$(CXX) $(CXXFLAGS) -o test_parser test_parser.cpp parser.o lexer.o

test_lua_env: test_lua_env.cpp lua_env.o
	$(CXX) $(CXXFLAGS) $(LUA_CFLAGS) -o test_lua_env test_lua_env.cpp lua_env.o $(LUA_LIBS)

test_executor: test_executor.cpp executor.o lua_env.o parser.o lexer.o
	$(CXX) $(CXXFLAGS) $(LUA_CFLAGS) -o test_executor test_executor.cpp executor.o lua_env.o parser.o lexer.o $(LUA_LIBS) -lpthread

test: test_lexer test_parser test_lua_env test_executor
	./test_lexer && ./test_parser && ./test_lua_env && ./test_executor

clean:
	rm -f *.o wisp test_lexer test_parser test_lua_env test_executor

.PHONY: test clean
