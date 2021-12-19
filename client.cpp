#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <sys/stat.h>

#include "tracker.h"
#include "utils.h"
#include "download.h"
#include "peer.h"
#include "listen.h"

using namespace std;

int main(int argc, char *argv[]) {
    if (argc < 3 || argc > 4) {
        puts("Usage: ./client <torrent file> <output path / file to seed> <upload limit in kB/sec> (optional)");
        exit(0);
    }
	
	int upLimit = -1;
	
	if (argc == 4) {
		printf("Entering BitTyrant mode with upload limit %d kB/sec\n", atoi(argv[3]));
		upLimit = atoi(argv[3]) * 10240; // kB/sec to bytes/round (10 seconds)
	}
	
    string torrentFile (argv[1]);
    string outputPath (argv[2]);

    ifstream f(outputPath.c_str());
    bool seeder = f.good();

    if (seeder) {
        puts("File exists, entering seeding mode");
    }

    /* Parse the torrent file */

    TorrentFile file (argv[1]);

    printf("Got torrent file:\n");
    printf("  announce: %s\n", file.announce.c_str());
    printf("  comment: %s\n", file.comment.c_str());
    printf("  name: %s\n", file.name.c_str());
    printf("  pieces count: %d\n", (int) file.pieceHashes.size());
    printf("  pieces length: %d\n", file.piecesLength);
	printf("  file length: %d\n", file.fileLength);

    vector<Piece*> pieces;

    int i = 0;
    for (auto hash : file.pieceHashes) {
		
		if (i == (int)file.pieceHashes.size() - 1) {
			pieces.push_back(new Piece(hash, i, file.fileLength % file.piecesLength));
		} else {
			pieces.push_back(new Piece(hash, i, file.piecesLength));
		}
        i++;
    }
	
    if (seeder) {
        file.readFile(argv[2], pieces);
	}
	
	/* Open listen socket */
	
	int listenPort = 6880;
	int listenSock = bindTo(listenPort);
	
	while (listenSock == -1 && listenPort < 6890) {
		listenPort++;
		listenSock = bindTo(listenPort);
	}

    printf("Bound to port %d\n", listenPort);
	
	if (listenSock == -1) {
		puts("Failed to bind to listen port");
		exit(0);
	}
	
	/* Tracker request */

    vector<Peer*> peers = trackerRequest(file, listenPort);

	printf("Received %d peers\n\n", (int) peers.size());
    
	/* Mutexes for thread safety*/
	
	mutex peer_lock;
	mutex piece_lock;
	
	/* Dispatch listener */

    thread listenThread (dispatchListen, ref(peers), listenSock, &pieces, file.hash, ref(peer_lock), ref(piece_lock));
    listenThread.detach();

    /* Dispatch thread to handle stuff */
	
	if (upLimit == -1) { // Normal mode
		thread handlerThread(maintainAllPeers, ref(peers), ref(pieces), ref(peer_lock), ref(piece_lock));
		handlerThread.detach();
	} else { // BitTyrant mode
		thread handlerTyrant(maintainAllPeersTyrant, ref(peers), ref(pieces), ref(peer_lock), ref(piece_lock), upLimit);
		handlerTyrant.detach();
	}

    /* Dispatch worker threads to download pieces */

    if (!seeder) {
        download(pieces, peers, file.hash, ref(peer_lock), ref(piece_lock));
        write_pieces(pieces, outputPath, ref(piece_lock));
    } else {
        seed(pieces, peers, ref(peer_lock), ref(piece_lock));
    }

    return 0;
}

