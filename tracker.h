#include <iostream>
#include <endian.h>
#include "net.h"
#include "utils.h"
#include "peer.h"
#pragma once

using namespace std;

//checks if a peer is already present in the peer list
bool is_in(vector<Peer*> p_l, Peer *p) {
    for (int i= 0; i < (int)p_l.size(); i++) {
        Peer *curr= p_l.at(i);
        if ((curr->getAddress()).compare(p->getAddress()) ==0 && curr->getPort() == p->getPort())
            return true;
    }
    return false;
}

vector<Peer*> trackerRequestUdp(TorrentFile &file, int listenPort) {
    puts("\nSending UDP tracker request");
    /* Parse URL */
    string url = file.announce;

    url = url.substr(6, url.size() - 6);
    auto pos = url.rfind("/announce");
    if (pos != string::npos) {
        url = url.substr(0, pos);
    }

    int port = 0;

    pos = url.rfind(":");
    if (pos != string::npos) {
        string temp = url.substr(pos + 1, url.size() - 1);
        port = stoi(temp);

        url = url.substr(0, pos);
    }

    string address = getIPFromName(url, IPPROTO_UDP);

    /* Other stuff */

    srand(time(NULL));

    uint32_t id = (uint32_t) rand(); // Id for this session

    uint64_t connId = htobe64(0x41727101980);
    uint32_t action = htonl(0);
    uint32_t transactionId = htonl(id);

    uint8_t buffer[16];
    memcpy(&buffer[0], &connId, 8);
    memcpy(&buffer[8], &action, 4);
    memcpy(&buffer[12], &transactionId, 4);


    UdpStream stream (address, port);

    /* Send tracker connection */

    stream.send((uint8_t *) &buffer, 16);

    /* Recieve tracker reply */

    string reply = stream.recv();

    if (reply.size() < 16) {
        puts("Error in tracker reply");
        exit(0);
    }

    /* Parse tracker reply buffer */

    uint8_t *replyRaw = (uint8_t *) reply.c_str();

    memcpy(&action, &replyRaw[0], 4);
    memcpy(&transactionId, &replyRaw[4], 4);
    memcpy(&connId, &replyRaw[8], 8);

    action = ntohl(action);
    transactionId = ntohl(transactionId);
    connId = be64toh(connId);

    // TODO: Validate fields

    /* Send announcement to tracker */

    connId = htobe64(connId);
    action = htonl(1);

    uint8_t infoHashRaw[20];

    for (int i = 0; i < 20; i++) {
        infoHashRaw[i] = file.hash.c_str()[i];
    }

    string peerId = getId(); 
    uint64_t downloaded = htobe64(0);
    uint64_t left = htobe64(file.piecesLength); // TODO: Correct value
    uint64_t uploaded = htobe64(0);
    uint32_t event = htonl(2); // TODO: ???
    uint32_t ip = htonl(0); // TODO: Fill this field
    uint32_t key = htonl(rand()); // TODO: What is this
    uint32_t num_want = htonl(-1);
    uint16_t listen_port = htons(listenPort);

    uint8_t buffer2[98];
    memcpy(&buffer2[0], &connId, 8);
    memcpy(&buffer2[8], &action, 4);
    memcpy(&buffer2[12], &transactionId, 4);

    memcpy(&buffer2[16], &infoHashRaw, 20); // Info hash

    memcpy(&buffer2[36], &transactionId, 5); // Peer id
    memcpy(&buffer2[56], &downloaded, 8);
    memcpy(&buffer2[64], &left, 8);
    memcpy(&buffer2[72], &uploaded, 8);
    memcpy(&buffer2[80], &event, 4);
    memcpy(&buffer2[84], &ip, 4); // my ip
    memcpy(&buffer2[88], &key, 4);
    memcpy(&buffer2[92], &num_want, 4);
    memcpy(&buffer2[96], &listen_port, 2);

    stream.send((uint8_t *) &buffer2, 98);

    /* Recieve announcement reply from tracker */

    reply = stream.recv();

    uint32_t interval;
    uint32_t leechers;
    uint32_t seeders;

    uint8_t *replyRaw2 = (uint8_t *) reply.c_str();
    memcpy(&action, &replyRaw2[0], 4);
    memcpy(&transactionId, &replyRaw2[4], 4);
    memcpy(&interval, &replyRaw2[8], 4);
    memcpy(&leechers, &replyRaw2[12], 4);
    memcpy(&seeders, &replyRaw2[16], 4);

    action = ntohl(action);
    transactionId = ntohl(transactionId);
    interval = ntohl(interval);
    leechers = ntohl(leechers);
    seeders = ntohl(seeders);

    /*printf("Recieved reply of size %d\n", (int) reply.size());
    printf(" > action: %u\n", action);
    printf(" > transaction id: %u\n", transactionId);
    printf(" > interval: %u\n", interval);
    printf(" > leechers: %u\n", leechers);
    printf(" > seeders: %u\n", seeders);*/

    int numPeers = ((int) reply.size() - 20) / 6;

    vector<Peer*> peers;

    for (int i = 0; i < numPeers; i++) {
        uint8_t *replyBuf = (uint8_t *) reply.c_str();
        uint32_t peerIP = 0;
        uint16_t peerPort = 0;

        memcpy(&peerIP, &replyBuf[i*6 + 20], 4);
        memcpy(&peerPort, &replyBuf[i*6 + 20 + 4], 2);

        peerIP = ntohl(peerIP);
        peerPort = ntohs(peerPort);

        string peerAddress = decodeAddress(peerIP);
        Peer *p= new Peer(peerAddress, (short) peerPort);
        if (!(peerAddress == "127.0.0.1" && peerPort == listenPort) && is_in(peers, p) == false)
            peers.push_back(p);
    }

    stream.close();

    return peers;
}

string encode_str(const char *str) {
    if (str == NULL || *str =='\0')
        return NULL;

    int bytes= 0;
    //int len = strlen(str);
    int len = 20;

    //maximum buffer size will be every character being escaped

    char *encoded= (char*) malloc(len * 3 + 1);
    memset(encoded, '\0', len * 3 + 1);

    const char *hex = "0123456789ABCDEF";
    const char *no_escape= "AaBbCcDdEeFfGgHhIiJjKkLlMmNnOoPpQqRrSsTtUuVvWwXxYyZz-._~";

    for (int i= 0; i < len; i++) {
        //encode current character:
        char enc[4];
        unsigned char c= str[i];
        memset(enc, '\0', 4); 
        if (strchr(no_escape, c) != NULL) {
            enc[0]= c;
            memcpy(encoded + bytes, enc, 1);
            bytes += 1;
        } else {
            enc[0]= '%';
            enc[1]= hex[c >> 4];
            enc[2]= hex[c & 15];
            memcpy(encoded + bytes, enc, 3);
            bytes += 3;
        }
    }

    return string(encoded, bytes);
}

vector<Peer*> trackerRequestHttp(TorrentFile &file, int listenPort) {

/**Create tracker request**/
    /* Parse URL */
    string url= file.announce;
    //if announce field isnt HTTP, find one in announce list that is
    auto url_list = file.announceList;
    //only iterate thru announce list if current announce ISNT HTTP and there are elements in announce list
    if (url.find("http") == string::npos && url_list.size() != 0) {
        //find an http announce tracker link
        for (int i= 0; i < (int) url_list.size(); i++) {
            bencode::string u= get<string>(get<bencode::list>(url_list[i])[0]);
            if (u.find("http") != string::npos) {
                url= u;
                break;
            }
        }
    }
    //url = url.substr(6, url.size() - 6);
    auto pos = url.find("http://");
    if (pos != string::npos)
        url = url.substr(pos, url.substr(pos, url.length()).find("/announce"));
    
    int url_port = 0;

    pos = url.rfind(":");
    string ip_url;
    if (pos != string::npos) {
        string temp = url.substr(pos + 1, url.rfind("/"));
        url_port = stoi(temp);
        ip_url= url.substr(0, pos);
        ip_url= ip_url.substr(ip_url.rfind("/") + 1, pos);
    }
    string address= getIPFromName(ip_url, IPPROTO_TCP);

    //printf("\nURL:: %s|| IP & port of URL: %s:%d\n", url.c_str(), address.c_str(), url_port);
 
    //decode bencoded info_hash into SHA1 hash

    uint8_t info_hash[21];
    for (int i = 0; i < 20; i++) 
        info_hash[i] = file.hash.c_str()[i];

    info_hash[20]= '\0';

    string peer_id2 = getId();
    const char *peer_id = peer_id2.c_str();

    /* URL encode ONLY the portion of the URL following the / after the .com */
    string encoded_info_hash = encode_str((char*) info_hash);
    string encoded_peer_id = encode_str(peer_id);
    
    string event= "started"; //hardcoded event
    
    //create socket to connect to tracker
    int sock = 0;

    if ((sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0) {
        puts("error creating socket");
    }

    struct sockaddr_in serv_addr4;
    memset(&serv_addr4, 0, sizeof(serv_addr4));

    serv_addr4.sin_family = AF_INET;
    serv_addr4.sin_port = htons(url_port);
    struct addrinfo hints, *serv_addr6;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype= SOCK_STREAM;
    hints.ai_family= AF_INET6;
    hints.ai_protocol= IPPROTO_TCP;

    if (inet_pton(AF_INET, address.c_str(), &serv_addr4.sin_addr) <= 0) {
        ::close(sock);
        sock= socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
        if (getaddrinfo(address.c_str(), to_string(url_port).c_str(), &hints, &serv_addr6) < 0)
            perror("Failed to convert to ip address");
        else { //for IPV6 connections
            if (::connect(sock, serv_addr6->ai_addr, serv_addr6->ai_addrlen))
                perror("\nERROR IN CONNECT\n");
        }
    } else { //for IPV4 connections
        if (::connect(sock, (struct sockaddr *) &serv_addr4, sizeof(serv_addr4)))
            perror("\nERROR IN CONNECT\n");
    }


    puts("\nSending HTTP tracker request"); 

    int compact= 1;

    /*export GET URL*/

    string request = 
        "GET /announce?info_hash=" 
        + encoded_info_hash
        + "&peer_id="
        + encoded_peer_id
        + "&port="
        + to_string(listenPort)
        + "&left="
        + to_string(file.piecesLength)
        + "&downloaded=0&uploaded=0&compact="
        + to_string(compact)
        +"&event=started HTTP/1.1\r\nHost: "
        + ip_url
        + ":"
        + to_string(url_port)
        + "\r\nConnection: Keep-Alive\r\n\r\n";

    cout << "GET request: " << request << endl;

    ::send(sock, (char*) request.c_str(), request.size(), 0);

    //Handle HTTP response
    char buffer[4096];
    memset(&buffer, 0, 4096);

    int bytes_rec = ::recv(sock, (char *) &buffer, 4096, 0);

    if (bytes_rec < 0)
        perror("Error in recieve");

    string response = string((char *) &buffer, bytes_rec);

    if (response.rfind("Bad Request") != string::npos) {
        puts("Server returned 400 bad request");
        exit(0);
    }

    /* parse response */
    auto index = response.find("Content-Length");
    if (index == string::npos) {
        puts("Error in parsing response");
        exit(0);
    }

    response = response.substr(index, response.length());

    /* parse response */
    index = response.find('d');
    if (index == string::npos) {
        puts("Error in parsing response");
        exit(0);
    }

    response = response.substr(index, response.length());

    //decode bencoded tracker response 
    auto dict = get<bencode::dict>(bencode::decode(response));

    puts("Checking for failure");
    //if failure present, print message
    if (dict.count("failure reason") || dict.count("failure code")) {
        printf("\nfailure %s: %s", (dict.count("failure reason") != 0 ? "reason" : "code"), get<string>(dict[(dict.count("failure reason") != 0 ? "failure reason" : "failure code")]).c_str());
        exit(0);
    }

    //if warning present, print it 
    if (dict.count("warning message"))
        printf("\nWarning message after announcing to tracker: %s\n", get<string>(dict["warning message"]).c_str());

    //# of seconds 
    auto interval= get<bencode::integer>(dict["interval"]);
    
    string tracker_id= "";
    if (dict.count("tracker id"))
        tracker_id= get<string>(dict["tracker id"]);

    auto seeders= get<bencode::integer>(dict["complete"]), leechers= get<bencode::integer>(dict["incomplete"]);
    
    printf("Recieved reply of size %d\n", (int) response.length());
    printf(" > tracker id: %s\n", tracker_id.c_str());
    printf(" > interval: %d\n", (int) interval);
    printf(" > leechers: %d\n", (int) leechers);
    printf(" > seeders: %d\n", (int) seeders);

    //assemble vector list of peers returned by tracker response
    int numPeers = seeders + leechers;
    vector<Peer*> peers;

    char * peer_list;

    try {
        peer_list = (char *) get<string>(dict["peers"]).c_str();
    } catch (exception& e) {
        puts("ERROR: Failed to decode compact format, trying non-compact");
        compact = 0;
    }

    if (compact) {
        //compact mode means peers list is replaced by peers string with 6 bytes per peer
        for (int i= 0; i < numPeers; i++) {
            char peer[6];
            memcpy(peer, peer_list + (i*6), 6);
            uint32_t peerIP;
            uint16_t peerPort;
            memcpy(&peerIP, peer, 4);
            memcpy(&peerPort, peer + 4, 2);
            peerIP= ntohl(peerIP);
            peerPort= ntohs(peerPort);
            string peerAddress= decodeAddress(peerIP);
            Peer *p= new Peer(address, (short)peerPort);
            if (!(peerAddress == "127.0.0.1" && peerPort == listenPort) && is_in(peers, p) == false) {
                printf("Adding peer with ip: %s port: %d\n", address.c_str(), peerPort);
                peers.push_back(p);       
            }
        }
    } else { //non-compact gives us a dict of peers
        //get peer dicts
        auto peers_list = get<bencode::list>(dict["peers"]);

        if (numPeers != (int) peers_list.size()) {
            puts("ERROR: Peers list size is different from numPeers");
        }

        for (int i = 0; i < (int) peers_list.size(); i++) {
            puts("Parsing peer...");
            string peerAddress= get<string>(get<bencode::dict>(peers_list.at(i))["ip"]);
            int peerPort= get<bencode::integer>(get<bencode::dict>(peers_list.at(i))["port"]);
            string peerId= get<string>(get<bencode::dict>(peers_list.at(i))["id"]);
            Peer *p= new Peer(peerAddress, (short)peerPort);
            if (!(peerAddress == "127.0.0.1" && peerPort == listenPort) && is_in(peers, p) == false) {
                printf("Adding peer with ip: %s port: %d id: %s\n", peerAddress.c_str(), peerPort, peerId.c_str());
                peers.push_back(p);
            }
        }
    }
    ::close(sock);
    return peers;
}

vector<Peer*> trackerRequest(TorrentFile& file, int listenPort) {
    if (file.announce.rfind("http") != string::npos)
        return trackerRequestHttp(file, listenPort);
    else
        return trackerRequestUdp(file, listenPort);
}

