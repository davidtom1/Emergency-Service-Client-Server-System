# STOMP Protocol Client-Server Implementation

## Overview

This project implements a client-server messaging system using the STOMP (Simple Text Oriented Messaging Protocol) protocol. The system consists of a Java-based server that supports both Thread-Per-Client (TPC) and Reactor patterns, and a C++ client application for managing event subscriptions and publishing.

## Project Structure

```
Assignment3/
├── client/              # C++ STOMP client implementation
│   ├── include/         # Header files
│   ├── src/            # Source files
│   ├── bin/            # Compiled binaries
│   └── makefile        # Build configuration
└── server/             # Java STOMP server implementation
    └── src/main/java/
        └── bgu/spl/net/
            ├── api/            # Protocol interfaces
            ├── impl/stomp/     # STOMP implementation
            └── srv/            # Server implementations
```

## Features

### Server Features
- STOMP protocol support (CONNECT, SUBSCRIBE, SEND, DISCONNECT, etc.)
- Two server architectures:
  - **Thread-Per-Client (TPC)**: Dedicated thread for each client connection
  - **Reactor**: Non-blocking I/O with thread pool
- Message routing and subscription management
- Multi-client support with connection handling

### Client Features
- Interactive command-line interface
- Event management from JSON files
- Commands supported:
  - `login <host:port> <username> <password>`
  - `join <channel>`
  - `exit <channel>`
  - `report <file>`
  - `summary <channel> <user> <file>`
  - `logout`
- Multi-threaded design (keyboard input + socket reading)
- Frame serialization/deserialization

## Requirements

### Server
- Java 8 or higher
- Maven

### Client
- C++11 or higher
- Boost libraries (boost_system, pthread)
- g++ compiler

## Build Instructions

### Building the Server

```bash
cd server
mvn clean package
```

### Building the Client

```bash
cd client
make
```

To clean build artifacts:
```bash
make clean
```

## Running the Application

### Starting the Server

```bash
java -jar server/target/server-1.0.jar <port> <server-type>
```

Parameters:
- `<port>`: Port number to listen on (e.g., 7777)
- `<server-type>`: Either `tpc` (Thread-Per-Client) or `reactor`

Example:
```bash
java -jar server/target/server-1.0.jar 7777 reactor
```

### Starting the Client

```bash
cd client/bin
./StompEMIClient
```

Once running, you can use the interactive commands to connect and interact with the server.

## STOMP Protocol Implementation

The implementation supports the following STOMP frames:
- **CONNECT**: Establish connection with server
- **CONNECTED**: Server acknowledgment of connection
- **SUBSCRIBE**: Subscribe to a channel
- **UNSUBSCRIBE**: Unsubscribe from a channel
- **SEND**: Send message to a channel
- **MESSAGE**: Receive message from subscribed channel
- **DISCONNECT**: Close connection
- **RECEIPT**: Acknowledge command execution
- **ERROR**: Error notification from server

## Authors

Assignment 3 - Systems Programming Course

## License

This project is part of an academic assignment.
