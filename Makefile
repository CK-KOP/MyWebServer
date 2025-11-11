CXX ?= g++

DEBUG ?= 1

ifeq ($(DEBUG), 1)
	CXXFLAG += -g
else
	CXXFLAG += -O2
endif

# 添加 include 路径
CXXFLAG += -I./third_party


server: main.cpp webserver.cpp config.cpp ./mydb/sql_connection_pool.cpp ./http/http_conn.cpp ./log/log.cpp ./timer/lst_timer.cpp ./third_party/picohttpparser/picohttpparser.c
	$(CXX) -o server $^ $(CXXFLAG) -lpthread -lmysqlclient

.PHONY : clean
clean:
	rm -r server
