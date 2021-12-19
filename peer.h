#include <stdlib.h>
#include <string>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <atomic>
#include <future>
#include <algorithm>
#include <mutex>

#pragma once

using namespace std;

// Peer message IDs
const char CHOKE = 0;
const char UNCHOKE = 1;
const char INTERESTED = 2;
const char NOT_INTERESTED = 3;
const char HAVE = 4;
const char BITFIELD = 5;
const char REQUEST = 6;
const char PIECE = 7;
const char CANCEL = 8;

class Peer {
public:
	bool am_interested, peer_interested, am_choking, peer_choking;
    bool connect_attempted { false };
    bool handshake_attempted { false };
    int requested_count { 0 };
	int connfd { 0 }; 
	string peer_bitfield;
	atomic<time_t> sent_time, recv_time;
    bool was_shaken { false };
	deque<Request*> recv_requests;
	mutex request_lock;
	
	bool tyrant_mode { false };
	int send_limit { 163840 }; // 16 KB, Eyeballed from the chart on the paper, times 10 seconds per round
	int bytes_sent { 0 };
	int bytes_received { 0 };
	int rounds_unchoked { 0 };

	int successive_request_failures { 0 };

	// N.B. At present this assumes port is passed in *host byte order*
	Peer(string construct_addr, uint16_t construct_port) {
		addr = construct_addr;
		port = construct_port;
		
		am_choking = true;
		peer_choking = true;
		
		am_interested = false;
		peer_interested = false;
		
		sent_time = time(0);
		recv_time = time(0);
	}
	
	Peer(string construct_addr, short construct_port, string construct_id) {
		addr = construct_addr;
		port = construct_port;
		id = construct_id;
		
		am_choking = true;
		peer_choking = true;
		
		am_interested = false;
		peer_interested = false;
		
		sent_time = time(0);
		recv_time = time(0);
	}

	Peer(int construct_connfd, string construct_addr, short construct_port) {
		addr = construct_addr;
		port = construct_port;
		
		connect_attempted = true;
		
		am_choking = true;
		peer_choking = true;
		
		am_interested = false;
		peer_interested = false;

        connfd = construct_connfd;
		
		sent_time = time(0);
		recv_time = time(0);
    }

    void connectAsync() {
        connect_attempted = true;
        auto temp = async(launch::async, &Peer::connect, this);
    }

    void handshakeAsync(Peer* peer, string hash, string peer_id, vector<Piece*>& pieces) {
        handshake_attempted = true;
        auto temp = async(launch::async, &Peer::handshake, peer, hash, peer_id, pieces);
    }

    bool connect() {
        connect_attempted = true;
        int sock = 0;
        struct sockaddr_in serv_addr;

        if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
            puts("error creating socket");
            return false;
        }

        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(port);

        if(inet_pton(AF_INET, addr.c_str(), &serv_addr.sin_addr) <= 0) {
            puts("Failed to convert ip address");
            return false;
        }

        fd_set set;
        FD_ZERO(&set);
        FD_SET(sock, &set);
		
        fcntl(sock, F_SETFL, O_NONBLOCK);
		
		struct timeval twosec;
		twosec.tv_sec = 2;
		twosec.tv_usec = 0;
		
		// socket will block for 2 seconds on read (ought to be more than long enough for a block to be reassembled by TCP)
		setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &twosec, sizeof(twosec));
		
        struct timeval timeout;
        timeout.tv_sec = 5;
        timeout.tv_usec = 0;

        ::connect(sock, (struct sockaddr *) &serv_addr, sizeof(serv_addr));
		
        if (select(sock + 1, NULL, &set, NULL, &timeout) == 1) {
            int so_error;
            socklen_t len = sizeof(so_error);

            getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len);

            if (so_error == 0) {
                connfd = sock;
                return true;
            } else {
                ::close(sock);
                return false;
            }
        }
		return false;
    }
	
	bool handle_shake(string hash_raw, string peer_id, vector<Piece*> pieces) {
		char buffer[68];
		
		int read_bytes = read(connfd, buffer, 68);
		
		if (read_bytes != 68) {
			printf("Bad handshake from %s:%d\n", addr.c_str(), port);
			close();
			return false;
		}
		
		recv_time = time(0);
		string handshake(buffer, 68);
		
		string response;
		response.append(1, (char) 19);
		response.append("BitTorrent protocol");
		response.append(8, (char) 0);
		response.append((char*) hash_raw.c_str(), 20);
		response.append(peer_id);
		
		if (response.compare(0, 20, handshake, 0, 20) != 0 || response.compare(28, 20, handshake, 28, 20) != 0) {
			printf("Bad handshake 2 from %s:%d\n", addr.c_str(), port);
            cout << response << endl;
			close();
			return false;
		}
		
		printf("Received handshake from %s:%d\n", addr.c_str(), port);
		write(connfd, response.data(), response.size());
		sent_time = time(0);
		
		handshake_attempted = true;
		was_shaken = true;
		send_bitfield(pieces);
		return true;
	}
	
	// Performs handshake
	// Takes in info_hash of the file(s) and our own peer_id
	// Returns socket descriptor, or 0 if either TCP or handshake fail
	bool handshake(string hash_raw, string peer_id, vector<Piece*> pieces) {
        handshake_attempted = true;
		string handshake; 
		handshake.append(1, (char) 19); // pstrlen
		handshake.append("BitTorrent protocol"); // pstr
		handshake.append(8, (char) 0); // reserved
		handshake.append((char*) hash_raw.c_str(), 20);
		handshake.append(peer_id);
		
		write(connfd, handshake.data(), handshake.size());
		sent_time = time(0);
		
		struct pollfd fd = {connfd, POLLIN, 0};
		
		// Wait 5 seconds
		if (poll(&fd, 1, 5000) != 1) {
			printf("Handshake timed out with peer at %s:%u\n", addr.c_str(), port);
            close();
			return false;
		}
		
		char reply_buff[68];
		read(connfd, reply_buff, 68);
		recv_time = time(0);
		
		string reply(reply_buff, 68);
		
		// Check pstrlen, pstr, and infohash are identical to ours
		if (handshake.compare(0,  20, reply, 0, 20) != 0 || 
			handshake.compare(28, 20, reply, 28, 20) != 0) {
			
			//printf("Handshake failed with peer at %s:%u\n", addr.c_str(), port);
            close();
			return false;
		}
		
		// N.B. this does not check the 8 reserved bytes for anything
		
		// Check that the ID tracker gave us matches the one we just received, but only if tracker gave us an id
		if (id.size() > 0) {
			if (id.compare(0, 20, reply_buff, 48, 20) != 0) {
				//printf("Handshake failed: IDs don't match\n Tracker: %s\n Peer: %s\n", id, reply_buff[48]);
                close();
				return false;
			}
		} else {
			// Store received ID if we didn't get one from tracker
			id.assign(&reply_buff[48]);
		}
		
        was_shaken = true;
		send_bitfield(pieces);
		return true;
	}

    bool try_recv(vector<Piece*>& pieces) {
		if (wasConnected()) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(connfd, &fds);

            timeval tv { 0, 0 };
            select(connfd + 1, &fds, nullptr, nullptr, &tv);

            if (FD_ISSET(connfd, &fds)) {				
				recv(pieces);
				recv_time = time(0);
				return true;
            }
        }
       
        return false;
    }
	
	// Call this when poll() says this peer sent something to us
	void recv(vector<Piece*>& pieces) {
		uint32_t length;
		int alive = read(connfd, &length, 4);
		
		if (alive != 4) {
			printf("Peer at %s:%d closed connection\n", addr.c_str(), port);
			close();
			return;
		}
		
		length = ntohl(length);
		
		// Keepalive messages have length 0 and no id
		if (length == 0) {
			printf("Received keepalive from %s:%d\n", addr.c_str(), port);
			
			// Nothing to do here; recv_time is updated in try_recv()
			
			return;
		}
		
		//printf("Incoming message of length %u from %s:%d\n", length, addr.c_str(), port);
		char mess_code;
		read(connfd, &mess_code, 1);
		
		switch(mess_code) {
			case CHOKE:
			    printf("Received choke from %s:%d\n", addr.c_str(), port);
				peer_choking = true;
				rounds_unchoked = 0;
				break;
			
			case UNCHOKE:
			    printf("Received unchoke from %s:%d\n", addr.c_str(), port);
				peer_choking = false;
				break;
			
			case INTERESTED:
			    printf("Received interested from %s:%d\n", addr.c_str(), port);
				peer_interested = true;
				break;
				
			case NOT_INTERESTED:
			    printf("Received not interested from %s:%d\n", addr.c_str(), port);
				peer_interested = false;
				break;
			
			case HAVE:
			    //printf("Received have from %s:%d\n", addr.c_str(), port);
				handle_have();
				break;
			
			case BITFIELD:
			    printf("Received bitfield from %s:%d\n", addr.c_str(), port);
				handle_bitfield(length);
				break;
			
			case REQUEST:
			    // printf("Received request from %s:%d\n", addr.c_str(), port);
				handle_request();
				break;
			
			case PIECE:
			    // printf("Received piece from %s:%d\n", addr.c_str(), port);
				handle_piece(pieces, length);
				break;
			
			case CANCEL:
			    // printf("Received cancel from %s:%d\n", addr.c_str(), port);
				handle_cancel();
				break;
			
			default:
			    printf("Received unrecognized message code %u from %s:%d\n", (unsigned char) mess_code, addr.c_str(), port);
                //close();
		}
	}
	
	void handle_request() {
		
		uint32_t piece_index;
		uint32_t block_offset;
		uint32_t block_length;
		
		read(connfd, &piece_index, 4);
		read(connfd, &block_offset, 4);
		read(connfd, &block_length, 4);
		
		piece_index = ntohl(piece_index);
		block_offset = ntohl(block_offset);
		block_length = ntohl(block_length);
		{
			lock_guard<mutex> lock(request_lock);
			recv_requests.push_back(new Request(piece_index, block_offset, block_length));
		}
		//printf("Received request from %s:%d for piece %d offset %d length %d\n", addr.c_str(), port, piece_index, block_offset, block_length);
		
	}
	
	void handle_cancel() {
		
		uint32_t piece_index;
		uint32_t block_offset;
		uint32_t block_length;
		
		read(connfd, &piece_index, 4);
		read(connfd, &block_offset, 4);
		read(connfd, &block_length, 4);
		
		piece_index = ntohl(piece_index);
		block_offset = ntohl(block_offset);
		block_length = ntohl(block_length);
		
		printf("Received cancel from %s:%d for piece %d offset %d length %d\n", addr.c_str(), port, piece_index, block_offset, block_length);

		{
			lock_guard<mutex> lock(request_lock);
			for (int i = recv_requests.size() - 1; i >= 0; i--) {
				if (recv_requests[i]->index == piece_index && recv_requests[i]->block == block_offset && recv_requests[i]->length == block_length) {
					recv_requests.erase(recv_requests.begin() + i);
					break;
				}
			}
		}
	}
	
	void handle_have() {
		
		uint32_t index;
		read(connfd, &index, 4);
		index = ntohl(index);
		
		int byte_index = index/8; // C rounds towards 0; this is := floor for nonzero indices
		int bit_index = index%8;
		
		unsigned char bit = 0b10000000;
		bit >>= bit_index;
		
		// If peer_bitfield is too small to represent the piece just reported, pad it with 0s
		if (byte_index >= (int) peer_bitfield.size()) {
			
			peer_bitfield.append(byte_index - peer_bitfield.size(), (char) 0);
			peer_bitfield.append(bit, 1);
		
		} else {
			
			// Update the relevant byte in peer_bitfield
			peer_bitfield[byte_index] = peer_bitfield[byte_index] | bit;
		}
	}
	
	void handle_bitfield(int length) {
		char* temp = (char*) malloc(length-1);
		// read(connfd, temp, length-1);
		::recv(connfd, temp, length-1, MSG_WAITALL);
		
        try {
		    peer_bitfield.assign(temp, length-1);
        } catch (const std::exception& e) {
            puts("EXCEPTION in handle_bitfield");
        }
		
		free(temp);
	}
	
	void handle_piece(vector<Piece*>& pieces, uint32_t length) {
		size_t blocklen = length - 9;
		char* temp = (char*) malloc(blocklen);
		bzero(temp, blocklen);
		uint32_t piece_index;
		uint32_t block_offset;
		
		fcntl(connfd, F_SETFL, 0);
		
		read(connfd, &piece_index, 4);
		read(connfd, &block_offset, 4);
		// int block_read = read(connfd, temp, blocklen);
		int block_read = ::recv(connfd, temp, blocklen, MSG_WAITALL );
		
		fcntl(connfd, F_SETFL, O_NONBLOCK);
		
		piece_index = ntohl(piece_index);
		block_offset = ntohl(block_offset);
		
		//printf("Read block of length %i for piece %u\n", block_read, piece_index);
		
		// Don't know if there's any other way to handle this gracefully aside from disconnecting
		if (block_read < (int) blocklen) {
			printf("Download from %s:%d too slow; closing connection\n", addr.c_str(), port);
			free(temp);
			close();
			return;
		}

		pieces[piece_index]->buffer.replace(block_offset, blocklen, temp, blocklen);

        int block_index = block_offset / 16384;
        pieces[piece_index]->blocks[block_index]->downloaded = true;
        pieces[piece_index]->blocks[block_index]->downloading = false;
		
		if (tyrant_mode) {
			bytes_received += blocklen;
		}
		
		free(temp);
	}
	
	void send_have(Piece* piece) {
        uint32_t len = htonl(5);
        uint8_t id = HAVE;
        uint32_t index = htonl(piece->index);

        uint8_t buffer[9];
        memcpy(&buffer[0], &len, 4);
        memcpy(&buffer[4], &id, 1);
        memcpy(&buffer[5], &index, 4);

        int sent = send(connfd, buffer, 9, MSG_NOSIGNAL);
        if (sent == -1) {
            printf("ERROR: Write fail; is the socked closed?\n");
            close();
        }
		sent_time = time(0);
    }

    void send_bitfield(vector<Piece*> pieces) {
        /*
        // Send a series of HAVE messages instead of bitfield
        for (Piece * piece : pieces) {
            if (piece->downloaded || rand() % 5 == 2) { // TODO: Remove probability
                send_have(piece);
            }
        }
        */
		
        uint8_t* bytes = (uint8_t *) malloc(pieces.size() / 8 + 1);
		bzero(bytes, pieces.size() / 8 + 1);
		
		bool have_pieces = false;
		
        for (Piece* piece : pieces) {
            if (piece->downloaded) {
				have_pieces = true;
                uint32_t byte_index = piece->index / 8;
                uint32_t bit_index = piece->index % 8;
                uint8_t mask = 0b10000000 >> bit_index;

                bytes[byte_index] |= mask;
            }
        }
		
		// If we don't have any pieces then there's no point
		if (!have_pieces) {
			free(bytes);
			return;
		}
		
		//printf("Sending bitfield to peer at %s:%i\n", addr.c_str(), port);
		
        uint32_t len = htonl(pieces.size() / 8 + 2);
        uint8_t id = BITFIELD;

        uint8_t* buffer = (uint8_t *) malloc(pieces.size() / 8 + 1 + 5);
        memcpy(&buffer[0], &len, 4);
        memcpy(&buffer[4], &id, 1);
        memcpy(&buffer[5], bytes, pieces.size() / 8 + 1);

        int sent = send(connfd, buffer, pieces.size() / 8 + 1 + 5, MSG_NOSIGNAL);
        if (sent == -1) {
            printf("ERROR: Write fail in send_bitfield; is the socket closed?\n");
            close();
        }
		
		sent_time = time(0);
        free(bytes);
        free(buffer);
    }
	
	// This will fulfill the oldest request sent from this peer
	void send_piece(vector<Piece*>& pieces) {
		
		lock_guard<mutex> lock(request_lock);
		
		if (recv_requests.empty()) {
			printf("Attempted to send piece to %s:%d, who did not request any\n", addr.c_str(), port);
			return;
		}
		
		Request* req = recv_requests.front();
		
		// Check that we have the piece, and that the request wouldn't overrun the piece buffer
		
		if (!pieces[req->index]->downloaded ||
			 (unsigned int) pieces[req->index]->size < (req->block + req->length)) {
			recv_requests.pop_front();
			return;
		}
		
		
		if (tyrant_mode && (int)(bytes_sent + req->length) > send_limit) { // Don't want to send over the send limit
			return;
		}
		
		
		uint32_t len = htonl(req->length + 9);
		uint8_t id = PIECE;
		uint32_t index = htonl(req->index);
		uint32_t block_offset = htonl(req->block);
		uint8_t* buffer = (uint8_t*) malloc(13 + req->length);
		
		memcpy(&buffer[0], &len, 4);
		memcpy(&buffer[4], &id, 1);
		memcpy(&buffer[5], &index, 4);
		memcpy(&buffer[9], &block_offset, 4);
		
		memcpy(&buffer[13], &(pieces[req->index]->buffer.data()[req->block]), req->length);
		
        //printf("Sending %d bytes piece index %d: ", req->length, req->index);
        for (int i = 13; i < 23; i++) {
            //printf("%02hhx", buffer[i]);
        }
        //printf("\n");

		int sent = send(connfd, buffer, 13 + req->length, MSG_NOSIGNAL);
		if (sent == -1) {
			printf("ERROR: Write fail in send_piece\n");
			close();
		}
		
		if (tyrant_mode) {
			bytes_sent += req->length;
		}
		
		sent_time = time(0);
		free(buffer);
		
		recv_requests.pop_front();
	}
	
    void downloadBlockAsync(Piece* piece, BlockMetadata* block) {
        if (!wasConnected())
            return;

        block->downloading = true;
        requested_count += 1;

        thread t (&Peer::downloadBlock, this, piece, block);
        t.detach();
    }

    void downloadBlock(Piece* piece, BlockMetadata* block) {
        if (!block->downloading) { // If not called asynchronously 
            block->downloading = true;
            requested_count += 1;
        }

        //printf("Requesting block %d from %s:%d\n", block->offset / 16384, addr.c_str(), port);

	    sendRequest(piece, block->offset, block->size);
		
        // Wait a maximum of 10 seconds for the block

        auto start = time(0);
        while(!block->downloaded && time(0) - start < 10) {
            usleep(10);
        }

        if (block->downloaded) {
            //printf("Downloading block %d piece %d from %s:%d SUCCEEDED\n", block->offset / 16384, piece->index, addr.c_str(), port);
            successive_request_failures = 0;
        } else {
            if(wasConnected()) {
                //printf("Downloading block %d piece %d from %s:%d failed\n", block->offset / 16384, piece->index, addr.c_str(), port);
                successive_request_failures += 1;

                if (successive_request_failures > 5) {
                    printf("Too many successive request failures from %s:%d, closing connection\n", addr.c_str(), port);
                    close();
                }
            }
        }
		
		if (requested_count > 0) {
			requested_count -= 1;
		}

        block->downloading = false;
		
        /*
		// print the first nonzero byte of the piece's buffer
		for (int i = 0; i < piece->size; i++) {
			char c = (char) piece->buffer[i];
			if (c == 0) continue;
			
			printf("Piece %i byte %i: %x\n", piece->index, i, (unsigned char) c);
			break;
		}
        */
    }
	
	// N.B. this will also clear the request log for this peer if we choke
	void sendChoking(bool choking) {
		if (choking) {
			lock_guard<mutex> lock(request_lock);
			printf("Sending choke to %s:%d\n", addr.c_str(), port);
			recv_requests.clear();
		} else {
			printf("Sending unchoke to %s:%d\n", addr.c_str(), port);
		}
		
		uint8_t buffer[5];
		uint32_t length = htonl(1);
		
		memcpy(&buffer, &length, 4);
		
		if (choking)
			buffer[4] = CHOKE;
		else
			buffer[4] = UNCHOKE;
		
		int sent = send(connfd, &buffer, 5, MSG_NOSIGNAL);
		if (sent == -1) {
            printf("ERROR: Write fail in sendChoking; is the socket closed?\n");
            close();
        }
		
		am_choking = choking;
		sent_time = time(0);
	}
	
    void sendInterested(bool interest) {
        puts("Sending interest message");
        am_interested = interest;

        uint8_t buffer[5];
        uint32_t length = htonl(1);

        memcpy(&buffer, &length, 4);

        if (interest)
            buffer[4] = INTERESTED;
        else
            buffer[4] = NOT_INTERESTED;

		int sent = send(connfd, &buffer, 5, MSG_NOSIGNAL);
		
		if (sent == -1) {
			printf("ERROR: Write fail in sendInterested; is the socket closed?\n");
			close();
		}
		
		sent_time = time(0);
    }
	
	// Send a Request; spec says to request 16 KB at a time
	// Returns whether send was successful
	bool sendRequest(Piece* piece, int offset, int size) {
		if (piece->size <= offset) {
			puts("Bad request: offset greater than piece length, nothing sent");
			return false;
		}
		
		uint8_t buffer[17];
		uint32_t length = htonl(13);
		
		memcpy(&buffer, &length, 4);
		
		buffer[4] = REQUEST;
		
		uint32_t piece_index = (uint32_t) piece->index;
		piece_index = htonl(piece_index);
		memcpy(&buffer[5], &piece_index, 4);
		
		uint32_t block_index = (uint32_t) offset;
		block_index = htonl(block_index);
		memcpy(&buffer[9], &block_index, 4);
		
		// Request 16 KB or however many bytes are between block_index and the end of the piece
		uint32_t block_size = (uint32_t) size;
		block_size = htonl(block_size);
		memcpy(&buffer[13], &block_size, 4);
		
		//int sent = write(connfd, &buffer, 17);
        int sent = send(connfd, &buffer, 17, MSG_NOSIGNAL);
        if (sent == -1) {
            printf("ERROR: Write fail in sendRequest; is the socket closed?\n");
            close();
        }

		sent_time = time(0);
		return (sent == 17);
	}
	
	void sendCancel(Piece* piece, int offset, int size) {
		if (piece->size <= offset) {
			puts("Bad cancel: offset greater than piece length, nothing sent");
			return;
		}
		
		uint8_t buffer[17];
		uint32_t length = htonl(13);
		
		memcpy(&buffer, &length, 4);
		
		buffer[4] = CANCEL;
		
		uint32_t piece_index = (uint32_t) piece->index;
		piece_index = htonl(piece_index);
		memcpy(&buffer[5], &piece_index, 4);
		
		uint32_t block_index = (uint32_t) offset;
		block_index = htonl(block_index);
		memcpy(&buffer[9], &block_index, 4);
		
		uint32_t block_size = (uint32_t) size;
		block_size = htonl(block_size);
		memcpy(&buffer[13], &block_size, 4);
		
		int sent = send(connfd, &buffer, 17, MSG_NOSIGNAL);
		if (sent == -1) {
			printf("ERROR: Write fail in sendCancel\n");
			close();
		}
		
		sent_time = time(0);
	}
	
	int numRequests() {
		lock_guard<mutex> lock(request_lock);
		return recv_requests.size();
	}
	
    string getAddress() {
       return addr; 
    }
	
    uint16_t getPort() {
        return port;
    }

    bool wasConnected() {
        return connfd > 0;
    }

    void close() {
        ::close(connfd);
        connfd = 0;
        requested_count = 0;

        was_shaken = false;

		peer_choking = true;
		
		am_interested = false;
		peer_interested = false;
		
		lock_guard<mutex> lock(request_lock);
		
		recv_requests.clear();
		nextRound();
		rounds_unchoked = 0;
		successive_request_failures = 0;
    }
	
	void nextRound() {
		bytes_received = 0;
		bytes_sent = 0;
		if (!peer_choking) {
			rounds_unchoked++;
		}
	}
	
	bool hasPiece(int index) {
        if (!wasConnected())
            return false;
		
		int byte_index = index / 8;
		int bit_index = index % 8;
		
		if (byte_index > (int) peer_bitfield.size()) {
			return false;
		}
		
		unsigned char bitmask = 0b10000000;
		bitmask >>= bit_index;
		
		return ((peer_bitfield[byte_index] & bitmask) == bitmask);
	}
	
	// does all timeout-related checks with this peer in both directions
	// spec says keepalives sent every two minutes so check if dead for 3 minutes to be safe
	// returns whether peer is still alive
	bool checkTime() {
		time_t ctime = time(0);
		
		if (difftime(ctime, recv_time) > 180) {
			printf("Peer at %s:%d timed out\n", addr.c_str(), port);
			close();
			return false;
		}
		
		if (difftime(ctime, sent_time) > 120) {
			uint32_t zeroes = 0;
			write(connfd, &zeroes, 4);
			sent_time = ctime;
			printf("Sending keepalive to %s:%d\n", addr.c_str(), port);
		}
		
		return true;
		
	}
	
private:
	string id;
	string addr;
	uint16_t port;
	
	// Converts relevant fields into sockaddr_in struct
	struct sockaddr_in get_address() {
		struct sockaddr_in out;
		struct in_addr sin_addr;
		int code = inet_aton(addr.c_str(), &sin_addr);
		
		if (code != 1) {
			printf("Failed to parse address: %s\n", addr.c_str());
			throw;
		}
		
		out.sin_family = AF_INET;
		out.sin_port = htons(port);
		out.sin_addr = sin_addr;
		
		return out;
		
	}
};

Peer* peer_optimistic { nullptr };

void optimisticUnchoke(vector<Peer*>& peers) {
    puts("Optimistic unchoking");

    if (peers.size() <= 1)
        return;

    int rand_index = (int) ((int) rand()) % ((int) peers.size() - 1);

    for (int i = 0; i < (int) peers.size(); i++) {
        Peer* peer = peers[rand_index]; 
        
        if (peer->peer_interested && peer->am_choking) {
            if (peer_optimistic != nullptr && !peer->am_choking)
                peer_optimistic->sendChoking(true);

            peer->sendChoking(false);
            peer_optimistic = peer;
        }

        rand_index += 1;
        if (rand_index >= (int) peers.size())
            rand_index = 0;
    }
}

// Processes timeouts and requests &c. now
// Consider moving try_recv() here as well
void maintainAllPeers(vector<Peer*>& peers, vector<Piece*>& pieces, mutex& peer_lock, mutex& piece_lock) {
	const int max_unchokes = 3;
	int unchokes = 0;
    auto start = time(0) - 30;

	while(true) {
		{
			lock_guard<mutex> lock(peer_lock);
			for (Peer* peer : peers) {
				//printf("Using peer %s:%d\n", peer->getAddress().c_str(), peer->getPort());
				if (peer->wasConnected()) {

					// Optimistically unchoke a peer
					if (time(0) - start >= 30) {
						start = time(0);
						optimisticUnchoke(peers);
					}
					
					// This doesn't currently do anything if a peer happens to time out, aside from closing the socket in checkTime()
					peer->checkTime();
					
					// Interested peers whom we are currently choking will be unchoked with 1/4 probability up to a max of max_unchokes
					if (unchokes < max_unchokes && peer->peer_interested && peer->am_choking /* && rand() % 4 == 2 */ ) {
						unchokes++;
						peer->sendChoking(false);
					} 
					
					// Choke any peers who we unchoked but who are not interested
					else if (!peer->am_choking && !peer->peer_interested) {
						unchokes--;
						peer->sendChoking(true);
					}
					
					else if (peer->numRequests() > 0) {
						int i = 0;
						lock_guard<mutex> lock(piece_lock);
						while (peer->numRequests() > 0 && i < 5) {
							peer->send_piece(pieces);
							i++;
						}
					}
					
				} else { // If peer was disconnected
					
					if (!peer->am_choking) {
						peer->am_choking = true;
						unchokes--;
					}
					
				}
			}
		}
        
		// In the interest of allowing other threads a chance to access peers
		usleep(1000);
	}
}

// Constant parameters from the BitTyrant paper
const double gamma = 0.1;
const double delta = 0.2;
const int r = 3;
const int roundlength = 10; // 10 seconds, as advised in the paper

struct ReciprocalHandler {
	Peer* peer;
	int download { 163840 }; // Expected download performance from peer in one round
	int upload { 163840 } ; // (Estimated) upload required for reciprocation in one round
};

// An alternate handler that unchokes according to BitTyrant's algorithm
void maintainAllPeersTyrant(vector<Peer*>& peers, vector<Piece*>& pieces, mutex& peer_lock, mutex& piece_lock, int upCap) {
	int upRate = 0;
	
    auto start = time(0);
	vector<ReciprocalHandler> reciprocal_handlers;
	{
		lock_guard<mutex> lock(peer_lock);
		for (Peer* peer : peers) {
			reciprocal_handlers.push_back(ReciprocalHandler {peer});
			peer->tyrant_mode = true;
		}
	}
	while(true) {
		
		while (time(0) - start < roundlength) { // Within a round
			{
				lock_guard<mutex> lock(peer_lock);
				
				for (Peer* peer : peers) {
					
					if (peer->wasConnected()) {
						
						peer->checkTime();
						
						if (!peer->am_choking && peer->numRequests() > 0) {
							lock_guard<mutex> lock(piece_lock);
							peer->send_piece(pieces);
						}
						
					} else {
						
						if (!peer->am_choking) {
							peer->am_choking = true;
						}					
						
					}
					
				}
			}
			
			usleep(1000);
		
		}
		
		// After a round
		start = time(0);
		upRate = 0;
		
		{
			lock_guard<mutex> lock(peer_lock);
			
			for (Peer* peer : peers) { // Add any new peers to reciprocal_handlers
				for (ReciprocalHandler rh : reciprocal_handlers) {
					if (rh.peer == peer) {
						continue;
					}
				}
				reciprocal_handlers.push_back(ReciprocalHandler {peer});
				peer->tyrant_mode = true;
			}
		}
		
		for (ReciprocalHandler rh : reciprocal_handlers) {
			// Update upload and download estimates according to the BitTyrant algorithm
			if (rh.peer->peer_choking) {
				rh.upload += (int) rh.upload*delta;
			} else {
				// Strictly speaking this will update based on how many bytes we've gotten in the past 10 seconds and 
				// not download rate per se, since they probably unchoked us in the middle of that interval
				rh.download = rh.peer->bytes_received; 
			}
			
			if (rh.peer->rounds_unchoked >= r) {
				rh.upload -= (int) rh.upload*gamma;
			}
			
			rh.peer->nextRound();
			
		}
		
		// sort in descending order of download/upload ratio to get the most bang for our buck
		sort(reciprocal_handlers.begin(), reciprocal_handlers.end(), [](ReciprocalHandler x, ReciprocalHandler y) {return ((double)x.download/(double)x.upload) > ((double)y.download/(double)y.upload);});
		
		for (ReciprocalHandler rh : reciprocal_handlers) {
			if (rh.peer->am_choking && rh.peer->peer_interested) {
				
				if (upCap - upRate > rh.upload) {
					rh.peer->sendChoking(false);
					rh.peer->send_limit = rh.upload;
					upRate += rh.upload;
				}
				
			} else {
				
				if (upCap - upRate > rh.upload && rh.peer->peer_interested) {
					upRate += rh.upload;
					rh.peer->send_limit = rh.upload;
				} else {
					rh.peer->sendChoking(true);
				}
				
			}
		}

	    usleep(10);	
	}
}
