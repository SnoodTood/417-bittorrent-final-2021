#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "utils.h"

int bindTo(int port) {
    struct sockaddr_in servaddr;

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {
        puts("Failed to create socket");
        exit(0);
    }

    int enable = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0) {
        puts("Error setting socket options");
        exit(0);
    }

    bzero(&servaddr, sizeof(servaddr));

    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(port);

    if ((bind(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr))) != 0) {
        return -1;
    }
    
    return sockfd;
}
void dispatchListen(vector<Peer*>& peers, int listenSock, vector<Piece*> *pieces, string hash, mutex& peer_lock, mutex& piece_lock) {
    struct sockaddr_in client;
    int sockfd = listenSock;

    if ((listen(sockfd, 5)) != 0) {
        puts("Listen failed");
        exit(0);
    }

    unsigned int len = sizeof(client);

    while (true) {
        int connfd = accept(sockfd, (struct sockaddr *) &client, &len);
        if (connfd < 0) {
            puts("Failed to accept client");
        } else {
            uint16_t port;

            char* ip = inet_ntoa(client.sin_addr);
            port = htons(client.sin_port);

            printf("Accepted new connection from %s:%d\n", ip, port);
			Peer* newPeer = new Peer(connfd, ip, port);
			
			lock_guard<mutex> lock(peer_lock);
			lock_guard<mutex> lock2(piece_lock);
			
			if (newPeer->handle_shake(hash, getId(), *pieces)) {
				
				peers.push_back( newPeer );
			}
        }
    }
}

