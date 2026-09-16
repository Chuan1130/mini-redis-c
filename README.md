# mini-redis-c

A lightweight Redis-like key-value server implemented in C.

This project recreates a simplified Redis-style in-memory database from scratch, focusing on the core ideas behind:

- TCP server and client communication
- RESP protocol parsing and response encoding
- In-memory key-value storage
- Non-blocking network handling
- Persistence and recovery
- Key expiration management

It is designed as a learning-oriented systems project with clear module boundaries, runtime notes, and archived legacy versions showing the step-by-step evolution of the implementation.

---

## 1. Project Overview

This project implements a simplified Redis-like server in C.

At a high level, the system works like this:

1. A client sends a command to the server over TCP.
2. The server reads the incoming bytes from the socket.
3. The bytes are parsed according to a Redis-style RESP protocol.
4. The command is dispatched to the in-memory database layer.
5. The database executes the operation on the key-value store.
6. If needed, the persistence layer records data for recovery.
7. The server encodes the result into a RESP-style response.
8. The client receives and prints the result.

The goal of the project is not to fully clone Redis, but to rebuild the core mechanisms manually in order to understand how a real in-memory database works internally.

---

## 2. Implemented Features

Currently implemented or covered by the project structure:

- **TCP server**
  - socket creation
  - bind / listen / accept
  - request / response handling

- **TCP client**
  - connect to the server
  - send commands
  - receive and display responses

- **RESP-style protocol support**
  - request parsing
  - response generation
  - handling of structured command payloads

- **In-memory key-value database**
  - internal storage layer
  - key lookup / insert / delete flow
  - modular database abstraction

- **Non-blocking I/O utilities**
  - reusable networking helpers
  - support for more practical socket handling

- **Persistence and recovery**
  - data written to persistent storage
  - recovery logic for rebuilding state after restart

- **Key expiration management**
  - support for expiring keys
  - expiration-related internal processing

- **Learning notes and archived legacy versions**
  - chapter-by-chapter notes in `docs/`
  - earlier implementation snapshots in `archive/`

---

## 3. Project Structure

```text
mini-redis-c/
├── .gitignore
├── Makefile
├── README.md
├── include/
│   ├── codec.h
│   ├── db.h
│   ├── nb.h
│   ├── persist.h
│   └── proto.h
├── src/
│   ├── codec.c
│   ├── db.c
│   ├── nb.c
│   ├── persist.c
│   ├── proto.c
│   ├── tcp_client.c
│   └── tcp_server.c
├── docs/
│   ├── ch00_notes.md
│   ├── ch01_notes.md
│   ├── ch02_notes.md
│   ├── ch03_notes.md
│   ├── ch04_notes.md
│   ├── ch05_notes.md
│   ├── ch06_notes.md
│   ├── ch07_notes.md
│   ├── ch08_notes.md
│   └── runtime_walkthrough.md
└── archive/
    ├── README.md
    ├── ch01_legacy/
    ├── ch02_legacy/
    ├── ch03_legacy/
    ├── ch04_legacy/
    ├── ch05_legacy/
    ├── ch06_legacy/
    ├── ch07_legacy/
    └── ch08_legacy/
