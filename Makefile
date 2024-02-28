all:
	g++ -std=c++17 pg_proxy.cpp postgreSqlProxy.cpp -o pg_proxy

clean:
	rm pg_proxy