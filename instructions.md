# Compilation and Execution Instructions

## Prerequisites

Before running the project, ensure you have the following installed:

### For Server
- Java Development Kit (JDK) 8 or higher
- Apache Maven
- Set `JAVA_HOME` environment variable

### For Client
- g++ compiler with C++11 support
- Boost libraries:
  - boost_system
  - pthread
- Make utility

## Compilation Instructions

### Compiling the Server

1. Navigate to the server directory:
   ```bash
   cd server
   ```

2. Compile using Maven:
   ```bash
   mvn clean compile
   ```

3. Package into JAR file:
   ```bash
   mvn package
   ```

   This will create `server-1.0.jar` in the `target/` directory.

### Compiling the Client

1. Navigate to the client directory:
   ```bash
   cd client
   ```

2. Create the bin directory if it doesn't exist:
   ```bash
   mkdir -p bin
   ```

3. Compile using make:
   ```bash
   make
   ```

   This will compile all source files and create the executable `StompEMIClient` in the `bin/` directory.

4. To clean previous builds:
   ```bash
   make clean
   ```

## Execution Instructions

### Running the Server

1. Navigate to the server directory (if not already there):
   ```bash
   cd server
   ```

2. Run the server with the desired configuration:
   ```bash
   java -jar target/server-1.0.jar <port> <server-type>
   ```

   **Parameters:**
   - `<port>`: The port number for the server to listen on (e.g., 7777)
   - `<server-type>`: Choose one of:
     - `tpc` - Thread-Per-Client architecture
     - `reactor` - Reactor pattern architecture

   **Examples:**
   ```bash
   # Run server on port 7777 with Reactor pattern
   java -jar target/server-1.0.jar 7777 reactor

   # Run server on port 8080 with Thread-Per-Client pattern
   java -jar target/server-1.0.jar 8080 tpc
   ```

### Running the Client

1. Navigate to the client bin directory:
   ```bash
   cd client/bin
   ```

2. Execute the client:
   ```bash
   ./StompEMIClient
   ```

3. The client will start and wait for your commands. You can now interact with it using the following commands:

## Client Commands

Once the client is running, use these commands:

### Login
Connect to the server:
```
login <host:port> <username> <password>
```
Example:
```
login localhost:7777 user1 pass123
```

### Join a Channel
Subscribe to a channel:
```
join <channel>
```
Example:
```
join /topic/game_updates
```

### Exit a Channel
Unsubscribe from a channel:
```
exit <channel>
```
Example:
```
exit /topic/game_updates
```

### Report Events
Upload events from a JSON file:
```
report <json-file-path>
```
Example:
```
report events.json
```

### Generate Summary
Create a summary for a user in a channel:
```
summary <channel> <user> <output-file>
```
Example:
```
summary /topic/game_updates user1 summary.txt
```

### Logout
Disconnect from the server:
```
logout
```

## Troubleshooting

### Server Issues

**Port already in use:**
- Choose a different port number
- Or stop the process using that port

**Java version error:**
- Verify Java 8+ is installed: `java -version`
- Check JAVA_HOME is set correctly

### Client Issues

**Compilation errors:**
- Ensure Boost libraries are installed:
  - Ubuntu/Debian: `sudo apt-get install libboost-all-dev`
  - macOS: `brew install boost`
- Verify g++ supports C++11: `g++ --version`

**Connection refused:**
- Ensure the server is running
- Verify the correct host and port
- Check firewall settings

**Linker errors:**
- Make sure pthread and boost_system libraries are available
- On some systems, you may need to install: `libboost-system-dev`

## Testing the System

1. Start the server in one terminal:
   ```bash
   cd server
   java -jar target/server-1.0.jar 7777 reactor
   ```

2. Start the client in another terminal:
   ```bash
   cd client/bin
   ./StompEMIClient
   ```

3. In the client terminal, connect to the server:
   ```
   login localhost:7777 testuser password123
   ```

4. Try subscribing to a channel:
   ```
   join /topic/test
   ```

5. You can start multiple clients in different terminals to test multi-user functionality.

## Notes

- The client uses multi-threading: one thread for keyboard input and one for reading from the socket
- Make sure to properly logout before closing the client to avoid connection leaks
- Event JSON files should follow the expected format (see event.h for structure)
- All client commands are case-sensitive
