#include <stdlib.h>
#include <string>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#include <future>
#include <algorithm>

#include "peer.h"
#include "net.h"
#include "utils.h"

#pragma once

using namespace std;

bool end_game = false;

// Index-Frequency Pair
struct IFP {
	unsigned int index;
	unsigned int frequency;
};

// Returns a list of indices in order of decreasing rarity, as inferred from our current list of peers
vector<int> find_rarest(vector<Piece*> pieces, vector<Peer*> peers) {
	vector<int> out;
	vector<IFP> freq;
	for (unsigned int i = 0; i < pieces.size(); i++) {
		freq.push_back( {i, 0});
		
		for (unsigned int j = 0; j < peers.size(); j++) {
			if (peers[j]->hasPiece(i)) {
				freq[i].frequency++;
			}
		}
		
	}
	
	sort(freq.begin(), freq.end(), [](IFP x, IFP y) {return x.frequency < y.frequency;});
	for (unsigned int i = 0; i < pieces.size(); i++) {
		out.push_back(freq[i].index);
	}
	
	return out;
}

void print_status(vector<Piece*>& pieces) {
    /*
    printf("\nDownloaded map (. = none, ~ = partial, ✓ = complete):");

    int i = 0;
    for (auto piece : pieces) {
        if (i % 80 == 0)
            printf("\n");
            
        bool partial = false;
        for (auto block : piece->blocks) {
            if (block->downloaded)
                partial = true;
        }

        if (piece->downloaded)
            printf("✓");
        else
            if (partial)
                printf("~");
            else
                printf(".");

        i++;
    }

    printf("\n");
    */

    int downloaded_pieces = 0;
    int total_pieces = 0;

    int downloaded_blocks = 0;
    int total_blocks = 0;

    for (auto piece : pieces) {
        if (piece->downloaded)
            downloaded_pieces += 1;

        total_pieces += 1;

        for (auto block : piece->blocks) {
            if (block->downloaded)
                downloaded_blocks += 1;

            total_blocks += 1;
        }
    }

    printf("Downloaded %d/%d blocks [", downloaded_blocks, total_blocks);
    //printf("%d/%d pieces [", downloaded_pieces, total_pieces);

    int res = 50;
    for (int i = 0; i < res; i++) {
        if (downloaded_blocks * res / total_blocks < i)
            printf("-");
        else
            printf("#");
    }

    printf("] %d%%", downloaded_blocks * 100 / total_blocks);
    printf("\r");

}

bool download_block(Piece* piece, BlockMetadata* block, Peer* peer) {
    if (!peer->am_interested) {
        peer->sendInterested(true); // Need to ensure we don't spam this
    } else {
        if (!peer->peer_choking
            && peer->hasPiece(piece->index)
            && !block->downloading) {
            
            peer->downloadBlockAsync(piece, block);

            return true;
        }
    }

    return false;
}

bool download_piece_endgame(Piece* piece, vector<Peer*>& peers) {
    bool complete = true;

    for (auto block : piece->blocks) {
        if (!block->downloaded)
            complete = false;

        if (!block->downloaded) {
            for (auto peer : peers) {
                if (peer->wasConnected() && peer->requested_count < 2) {
                    download_block(piece, block, peer);
                }
            }
        }
    }

    if (complete) {
		auto hash = SHA1(piece->buffer);

		if (hash.substr(0, 20).compare(piece->hash.substr(0, 20)) == 0) {
			//printf("Downloading piece %d COMPLETE\n", piece->index);
			piece->downloaded = true;
			
			// Tell everyone we have the piece
			for (auto peer : peers) {
                if (peer->wasConnected()) {
				    peer->send_have(piece);
                }
			}
			
			return true;
			
		} else { // Something got corrupted in transmission; throw out the whole thing
			
			printf("Downloading piece %d CORRUPTED\n", piece->index);
			for (auto block : piece->blocks) {
				block->downloaded = false;
			}
			
		}
		
	}
	
	return false;
}

bool download_piece(Piece* piece, vector<Peer*>& peers) {
    bool complete = true;

    for (auto block : piece->blocks) {
        if (!block->downloaded)
            complete = false;

        if (!end_game) {
            if (!block->downloaded && !block->downloading) {
                for (auto peer : peers) {
                    if (peer->wasConnected() && peer->requested_count < 10) {
                        if(download_block(piece, block, peer))
                            return true;
                    }
                }
            }
        }
    }

    if (complete) {
		auto hash = SHA1(piece->buffer);
		
		if (hash.substr(0, 20).compare(piece->hash.substr(0, 20)) == 0) {
			//printf("Downloading piece %d COMPLETE\n", piece->index);
			piece->downloaded = true;
			
			// Tell everyone we have the piece
			for (auto peer : peers) {
                if (peer->wasConnected()) {
				    peer->send_have(piece);
                }
			}
			
			return true;
			
		} else { // Something got corrupted in transmission; throw out the whole thing
			
			printf("Downloading piece %d CORRUPTED\n", piece->index);
			for (auto block : piece->blocks) {
				block->downloaded = false;
			}
			
		}
		
	}
	
	return false;
	
}

void seed(vector<Piece*>& pieces, vector<Peer*>& peers, mutex& peer_lock, mutex& piece_lock) {
    puts("Seeding file...");
    while (true) {
		{
			lock_guard<mutex> lock(peer_lock);
			lock_guard<mutex> lock2(piece_lock);
			for (Peer* peer : peers) {
				if (peer->wasConnected()) {
					peer->try_recv(pieces);
				}
			}
		}

		usleep(100);
    }
}

void write_pieces(vector<Piece*>& pieces, string path, mutex& piece_lock) {
    string buffer = "";
	lock_guard<mutex> lock(piece_lock);
    for (auto piece : pieces) {
        buffer.append(piece->buffer);
    }

    std::ofstream out(path);
    out << buffer;
    out.close();

    cout << "\nWrote downloaded file to " << path << endl;

    usleep(1000);
    exit(0);
}

int remaining_blocks(vector<Piece*>& pieces) {
    int remaining = 0;

    for (auto piece : pieces) {
        for (auto block : piece->blocks) {
            if (!block->downloaded)
                remaining += 1;
        }
    }

    return remaining;
}

int requested_blocks(vector<Piece*>& pieces) {
    int requested = 0;

    for (auto piece : pieces) {
        for (auto block : piece->blocks) {
            if (block->downloading && !block->downloaded)
                requested += 1;
        }
    }

    return requested;
}

void download(vector<Piece*>& pieces, vector<Peer*>& peers, string hash, mutex& peer_lock, mutex& piece_lock) {
    puts("Listening for messages");

    auto start = time(0);
	
	/* Initialize connections */
	{
		lock_guard<mutex> lock(peer_lock);
		
		for (Peer* peer : peers) {
			peer->connectAsync();
			
			if (peer->wasConnected()) {
				peer->handshakeAsync(peer, hash, getId(), pieces);
			}
		}
	}
	/* Wait one second for lazy bitfields to come in */
	
	start = time(0);
	while (time(0) - start < 1) {
		{
			lock_guard<mutex> lock(peer_lock);
			
			for (Peer* peer : peers) {
				
				if (peer->wasConnected()) {
					peer->try_recv(pieces);
				}
				
			}
		}
		
		usleep(100);
	}
	vector<int> priority_pieces;
	/* Calculate rarest pieces from everyone's bitfields */
	{
		lock_guard<mutex> lock(peer_lock);
		lock_guard<mutex> lock2(piece_lock);
		priority_pieces = find_rarest(pieces, peers);
	}
    /* Find a peer for each piece */

    puts("\nConnecting to peers...");
    srand(time(0));
    bool complete = false;

    /* Temporary timeout of 60 seconds */

    while (!complete) {
        start = time(0);
        while (time(0) - start < 3 && !complete) {
            complete = true;
			lock_guard<mutex> lock(peer_lock);
			lock_guard<mutex> lock2(piece_lock);
            /* Recieve from each peer */

            for (Peer* peer : peers) {
                if (!peer->connect_attempted)
                    peer->connectAsync();

                if (!peer->handshake_attempted && peer->wasConnected())
                    peer->handshakeAsync(peer, hash, getId(), pieces);

                if (peer->wasConnected()) {
                    peer->try_recv(pieces);
                }
            }

            /* Download from each peer */

            for (int priority_index : priority_pieces) { // In order of rarest first
				
				Piece* piece = pieces[priority_index];
				
                if (!piece->downloaded) {
                    complete = false;

                    if (!end_game) {
                        if (download_piece(piece, peers)) {
                            print_status(pieces);
                            break;
                        }
                    } else {
                        if (requested_blocks(pieces) < 5) {
                            if (download_piece_endgame(piece, peers)) {
                                print_status(pieces);
                                break;
                            }
                        }
                    }
                }
            }

            if (complete) {
                break;
            }

            //if (remaining_blocks(pieces) <= 20 && remaining_blocks(pieces) < requested_blocks(pieces)) {
            if (remaining_blocks(pieces) <= 10) {
                if (!end_game) {
                    puts("\nEntering end game mode...\n");
                    end_game = true;
                }
            }

            usleep(10);
        }
		
		usleep(100);
    }
}
