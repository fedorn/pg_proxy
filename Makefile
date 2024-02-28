all:
	g++ -std=c++17 pg_proxy.cpp PostgreSQLProxy.cpp -o pg_proxy

clean:
	rm pg_proxy