#include <iostream>
#include <fstream>
#include <string>

#include "deps/hash.h"
#include "deps/bencode.hpp"

#pragma once

using namespace std;

struct Request {
	Request(int i, int b, int l) {
		index = i;
		block = b;
		length = l;
	}
	
	unsigned int index;
	unsigned int block;
	unsigned int length;
	
};

struct BlockMetadata {
    BlockMetadata(int o, int s) {
        offset = o;
        size = s;
    }

    int offset;
    int size;
    bool downloading { false };
    bool downloaded { false };
};

struct Piece {
    Piece(string h, int i, int s) {
        hash = h;
        index = i;
		size = s;

        if (s % 16384 == 0) {
            for (int i = 0; i < s / 16384; i++) {
                blocks.push_back(new BlockMetadata(i*16384, 16384));
            }
        } else {
            for (int i = 0; i < s / 16384 + 1; i++) {
                blocks.push_back(new BlockMetadata(i*16384, 16384));
            }

            blocks.back()->size = size % 16384;
        }

        buffer.resize(size);
    }

    int index { -1 };
	int size;
    string hash { "" };
    string buffer { "" };
    bool downloaded { false };
    vector<BlockMetadata*> blocks;
};

string read_file(string path) {
    ifstream stream (path);
    std::stringstream buffer;
    buffer << stream.rdbuf();
    string contents = buffer.str();
    return contents;
}

string SHA1(string input) {
	char* outbuff[20];
	struct sha1sum_ctx* checksum = sha1sum_create(NULL, 0);
	sha1sum_finish(checksum, (const uint8_t*) input.data(), input.size(), (uint8_t*) outbuff);
	sha1sum_destroy(checksum);
	string out((const char*) outbuff, (size_t) 20);
	return out;
}

class TorrentFile {
public:
    string announce;
    bencode::list announceList;
    string comment;
    string name;
    string hash;
	
	int fileLength;
    int piecesLength;
    vector<string> pieceHashes;

    vector<string> urlList;

    TorrentFile(string path) {
        readTorrent(path);
    }
    
    void readFile(string path, vector<Piece*>& pieces) {
        string fileContents = read_file(path);

        int offset = 0;
        for (Piece* piece : pieces) {
            piece->buffer = fileContents.substr(offset, piece->size);
            piece->downloaded = true;

            for (auto block : piece->blocks) {
                block->downloaded = true;
            }

            offset += piece->size;
        }

        if (fileContents.size() % 16384 != 0)
            pieces.back()->size = fileContents.size() % 16384;
    }

    void readTorrent(string path) {
        string fileContents = read_file(path);

        auto data = bencode::decode(fileContents);
        auto dict = get<bencode::dict>(data);
        
        try {
            announce = get<string>(dict["announce"]);
        } catch (exception& e) {
            puts("Failed to get announce URL");
        }

        try {
            announceList= get<bencode::list>(dict["announce-list"]);
        } catch (exception& e) {
            puts("Failed to get announce-list URLs");
        }

        try {
            comment = get<string>(dict["comment"]);
        } catch (exception& e) {
            //puts("Failed to get comment");
        }
		
        auto info = get<bencode::dict>(dict["info"]);
        name = get<string>(info["name"]);
        piecesLength = get<bencode::integer>(info["piece length"]);
		
		try {
			fileLength = get<bencode::integer>(info["length"]);
		} catch (exception& e) {
			puts("No \"length\" field; closing");
			exit(0); // Multi-file torrents will break our client; this is fine
		}
		
        auto raw_buffer = get<string>(info["pieces"]);

        string raw_dict = bencode::encode(info);
        hash = SHA1(raw_dict);

        for (int i = 0; i < (int) raw_buffer.size() / 20; i++) {
           pieceHashes.push_back(raw_buffer.substr(i*20, (i+1)*20));
        }
    }
};

void* getSinAddr(addrinfo *addr)
{
    switch (addr->ai_family)
    {
        case AF_INET:
            return &(reinterpret_cast<sockaddr_in*>(addr->ai_addr)->sin_addr);

        case AF_INET6:
            return &(reinterpret_cast<sockaddr_in6*>(addr->ai_addr)->sin6_addr);
    }

    return NULL;
}

string getIPFromName(string name, int proto) {
    string address = "";

    /* Get address for url */

    addrinfo hints = {};
    hints.ai_flags = AI_CANONNAME;
    hints.ai_family = AF_INET;
    if (proto == IPPROTO_UDP)
        hints.ai_socktype = SOCK_DGRAM;
    else
        hints.ai_socktype= SOCK_STREAM;
    hints.ai_protocol = proto;

    addrinfo *res;

    int ret = getaddrinfo(name.c_str(), NULL, &hints, &res);
    if (ret != 0) {
        cerr << "getaddrinfo() failed: " << gai_strerror(ret) << "\n";
    } 

    char ip[INET6_ADDRSTRLEN];

    for(addrinfo *addr = res; addr; addr = addr->ai_next) {
        address = inet_ntop(addr->ai_family, getSinAddr(addr), ip, sizeof(ip));
    }

    freeaddrinfo(res);

    return address;
}

string getRandomHash() {
    string hash = "aaaaaaaaaaaaaaaaaaaa";

    for (int i = 0; i < 20; i++) {
        hash[i] = 'A' + (rand() % 26);
    }

    return hash;
}

string gId = "";

string getId() {
    if (gId == "") {
        srand(time(0));
        gId = getRandomHash();
    }

    return gId;
}

string decodeAddress(uint32_t encodedAddress) {
    string ret; 
    
    uint8_t byte = 0;

    memcpy(&byte, &((uint8_t *) &encodedAddress)[3], 1);
    ret.append(to_string(byte));
    ret.append(".");

    memcpy(&byte, &((uint8_t *) &encodedAddress)[2], 1);
    ret.append(to_string(byte));
    ret.append(".");

    memcpy(&byte, &((uint8_t *) &encodedAddress)[1], 1);
    ret.append(to_string(byte));
    ret.append(".");

    memcpy(&byte, &((uint8_t *) &encodedAddress)[0], 1);
    ret.append(to_string(byte));

    return ret;
}

