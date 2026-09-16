# ch08 说明（核心主线收束章）：
# 这个 Makefile 负责把当前项目编译成 server 和 client。
# 到这一章为止，server 仍然通过 db.* 使用存储层，
# 同时又额外引入了 persist.* 来实现最小快照持久化。
# 这说明项目已经从“带时间语义的字符串 KV”继续推进到“可恢复状态的本地数据库”。

CC=gcc
CFLAGS=-Wall -Wextra -O2 -g -Iinclude

all: server client

server: src/tcp_server.c src/proto.c src/nb.c src/codec.c src/db.c src/persist.c include/proto.h include/nb.h include/codec.h include/db.h include/persist.h
	$(CC) $(CFLAGS) src/tcp_server.c src/proto.c src/nb.c src/codec.c src/db.c src/persist.c -o server

client: src/tcp_client.c src/proto.c include/proto.h
	$(CC) $(CFLAGS) src/tcp_client.c src/proto.c src/codec.c -o client


clean:
	rm -f server client
