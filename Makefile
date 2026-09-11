all:
	clang++ -std=c++2a -march=native -Weverything -Wno-c++20-extensions -Wno-missing-prototypes -Wno-c++98-compat main.cpp -o speecher -lcurl -lportaudio
