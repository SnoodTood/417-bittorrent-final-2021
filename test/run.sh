# To use this, npm -g bittorrent-tracker

#gnome-terminal -- bittorrent-tracker

clear

cd .. && make clean && make && clear && cd test 

gnome-terminal -- ../client test.torrent output.jpg file.jpg

sleep 3

../client test.torrent output.jpg

make clean

