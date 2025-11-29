CXX ?= g++

DEBUG ?= 1

ifeq ($(DEBUG), 1)
	CXXFLAG += -g
else
	CXXFLAG += -O2
endif

# 添加 include 路径
CXXFLAG += -I./third_party


server: main.cpp webserver.cpp subreactor.cpp config.cpp ./mydb/sql_connection_pool.cpp ./http/http_conn.cpp ./log/log.cpp ./timer/lst_timer.cpp ./utils/utils.cpp ./third_party/picohttpparser/picohttpparser.c
	$(CXX) -o server $^ $(CXXFLAG) -lpthread -lmysqlclient -std=c++14

.PHONY : clean
clean:
	rm -r server
