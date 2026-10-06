CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2
LDLIBS   := -lutil

# 注意：llm_web 用了 <filesystem>，GCC 8 及更早要给它加 -lstdc++fs
all: llm_shell llm_web

llm_shell: llm_shell.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDLIBS)

llm_web: llm_web.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

# 跑一遍回归测试（要 python3 + node；自己起测试服务，不碰 8093）
test: llm_web
	sh tests/run_all.sh

clean:
	rm -f llm_shell llm_web

.PHONY: all clean test