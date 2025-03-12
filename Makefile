CXX ?= g++

DEBUG ?= 1

ifeq ($(DEBUG), 1)
	CXXFLAG += -g
else
	CXXFLAG += -O2
endif

server: main.cpp webserver.cpp config.cpp ./mydb/sql_connection_pool.cpp ./http/http_conn.cpp ./log/log.cpp ./timer/lst_timer.cpp
	$(CXX) -o server $^ $(CXXFLAG) -lpthread -lmysqlclient

.PHONY : clean
clean:
	rm -r server
