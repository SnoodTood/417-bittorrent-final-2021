#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <fcntl.h>
#include <netdb.h>
#include <thread>
#include <cstring>
#include <string>

#pragma once

using namespace std;

class TcpStream {
public:
    TcpStream(string ip, int port) {
        mIP = ip;
        mPort = port;
    }

    ~TcpStream() {
        if (mSockFd > 0)
            ::close(mSockFd);
    }

    bool connect() {
        int sock = 0;
        struct sockaddr_in serv_addr;

        if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
            puts("error creating socket");
            return false;
        }

        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(mPort);

        if(inet_pton(AF_INET, mIP.c_str(), &serv_addr.sin_addr) <= 0) {
            puts("Failed to convert ip address");
            return false;
        }

        fd_set set;
        FD_ZERO(&set);
        FD_SET(sock, &set);

        fcntl(sock, F_SETFL, O_NONBLOCK);

        struct timeval timeout;
        timeout.tv_sec = 3;
        timeout.tv_usec = 0;

        ::connect(sock, (struct sockaddr *) &serv_addr, sizeof(serv_addr));

        if (select(sock + 1, NULL, &set, NULL, &timeout) == 1) {
            int so_error;
            socklen_t len = sizeof(so_error);

            getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len);

            if (so_error == 0) {
                //printf("Connection succeeded, set connfd to %d\n", mConnFd);
                mConnFd = sock;
                return true;
            } else {
                ::close(sock);
                return false;
            }
        }
    }

    void send(string data) {
        ::send(mConnFd, (const char *) data.c_str(), data.size(), 0);
    }

    string recv() {
        char buffer[4096];
        memset(&buffer, 0, 4096);

        int n = ::recv(mConnFd, (char *) &buffer, 4096, 0);

        if (n <= 0)
            puts("Error in recieve");

        return string ((char *) &buffer, n);
    }

    void close() {
        ::close(mConnFd);
    }

private:
    string mIP;
    int mPort;

    int mSockFd { 0 };
    int mConnFd { 0 };
};

class UdpStream {
public:
    UdpStream(string address, int port) {
        mIP = address;
        mPort = port;

        int sockfd;
        struct hostent *server;

        sockfd = socket(AF_INET, SOCK_DGRAM, 0);
        if (sockfd < 0) {
            puts("ERROR opening socket");
            exit(0);
        }

        server = gethostbyname(address.c_str());
        if (server == NULL) {
            puts("Failed to get host from name");
            exit(0);
        }

        bzero((char *) &servaddr, sizeof(servaddr));
        servaddr.sin_family = AF_INET;
        bcopy((char *)server->h_addr, (char *)&servaddr.sin_addr.s_addr, server->h_length);
        servaddr.sin_port = htons(port);

        mSockFd = sockfd;
    }

    void send(uint8_t *data, int size) {
        int n = ::sendto(mSockFd, (const char *) data, size, 0, (const struct sockaddr *) &servaddr, sizeof(servaddr));
        if (n < 0)
            puts("Error in UDP send");
    }

    void send(string data) {
        int n = ::sendto(mSockFd, (const char *) data.c_str(), data.size(), 0, (const struct sockaddr *) &servaddr, sizeof(servaddr));
        if (n < 0)
            puts("Error in UDP send");
    }

    string recv() {
        int n;
        char buffer[4097];
        memset(&buffer, 0, 4097);
        unsigned int len = sizeof(servaddr);

        n = recvfrom(mSockFd, (char *)&buffer, 4096, 0, (struct sockaddr *) &servaddr, &len);

        if (n <= 0) {
            puts("Error in UDP recieve");
            return "";
        }

        return string ((char *) &buffer, n);
    }

    void close() {
        ::close(mSockFd);
    }

private:
    string mIP;
    int mPort;

    int mSockFd { -1 };

    struct sockaddr_in servaddr;
};

string UDPSendRecv(string ip, int port, string data) {
    struct sockaddr_in servaddr;
	int s;
    unsigned int slen = sizeof(servaddr);
	char buffer[4096];

	if ((s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1)
	{
        puts("Failed to create socket");
        exit(0);
	}

	memset((char *) &servaddr, 0, sizeof(servaddr));
	servaddr.sin_family = AF_INET;
	servaddr.sin_port = htons(port);
	
	if (inet_aton(ip.c_str(), &servaddr.sin_addr) == 0) 
	{
        puts("Failed to convert ip adress");
		exit(0);
	}

    cout << "Sending to " << ip << endl;
    ::sendto(s, data.c_str(), data.size(), 0, (struct sockaddr *) &servaddr, slen);

    puts("Recieving...");
    int len = recvfrom(s, (char *) &buffer, 4096, MSG_WAITALL, (struct sockaddr *) &servaddr, &slen);

    buffer[len] = '\0';

    close(s);

    string ret = buffer;
    return ret;
   
}

string getRequest(string host, string url, int port, string data) {
    char buffer[BUFSIZ];
    enum CONSTEXPR { MAX_REQUEST_LEN = 1024};
    char request[MAX_REQUEST_LEN];
    char request_template[] = "GET / HTTP/1.1\r\nHost: %s\r\n\r\n";
    struct protoent *protoent;
    char *hostname = (char *) host.c_str();
    in_addr_t in_addr;
    int request_len;
    int socket_file_descriptor;
    ssize_t nbytes_total, nbytes_last;
    struct hostent *hostent;
    struct sockaddr_in sockaddr_in;
    unsigned short server_port = port;

    request_len = snprintf(request, MAX_REQUEST_LEN, request_template, url.c_str(), data);
    if (request_len >= MAX_REQUEST_LEN) {
        puts("Request too large");
        exit(EXIT_FAILURE);
    }

    /* Build the socket. */
    protoent = getprotobyname("tcp");
    if (protoent == NULL) {
        puts("Error getting protocol by name");
        exit(EXIT_FAILURE);
    }
    socket_file_descriptor = socket(AF_INET, SOCK_STREAM, protoent->p_proto);
    if (socket_file_descriptor == -1) {
        puts("Socket creation error");
        exit(EXIT_FAILURE);
    }

    hostent = gethostbyname(hostname);
    if (hostent == NULL) {
        puts("Error getting host by name");
        exit(EXIT_FAILURE);
    }

    in_addr = inet_addr(inet_ntoa(*(struct in_addr*)*(hostent->h_addr_list)));

    if (in_addr == (in_addr_t)-1) {
        puts("Error in inet_addr");
        exit(EXIT_FAILURE);
    }

    sockaddr_in.sin_addr.s_addr = in_addr;
    sockaddr_in.sin_family = AF_INET;
    sockaddr_in.sin_port = htons(server_port);

    puts("Attempting connection");

    if (connect(socket_file_descriptor, (struct sockaddr*) &sockaddr_in, sizeof(sockaddr_in)) == -1) {
        perror("connect");
        exit(EXIT_FAILURE);
    }

    puts("Connected");

    /* Send HTTP request. */
    nbytes_total = 0;
    while (nbytes_total < request_len) {
        nbytes_last = write(socket_file_descriptor, request + nbytes_total, request_len - nbytes_total);
        if (nbytes_last == -1) {
            perror("write");
            exit(EXIT_FAILURE);
        }
        nbytes_total += nbytes_last;
    }

    /* Read the response. */
    puts("Reading response");
    while ((nbytes_total = read(socket_file_descriptor, buffer, BUFSIZ)) > 0) {
        write(STDOUT_FILENO, buffer, nbytes_total);
    }

    puts("Read response");
    if (nbytes_total == -1) {
        perror("read");
        exit(EXIT_FAILURE);
    }

    close(socket_file_descriptor);
    exit(EXIT_SUCCESS);
}
