/*
 * StompClient.cpp
 * ---------------
 * This is the main entry point for the C++ client application.
 *
 * ARCHITECTURE OVERVIEW:
 * The client uses two threads running concurrently:
 *
 *   1. Main thread  -> runs process_keyboard_input()
 *      Reads commands typed by the user (login, join, report, etc.),
 *      translates them into STOMP frames, and sends them to the server.
 *
 *   2. Socket thread -> runs read_from_socket()
 *      Continuously listens on the TCP socket for incoming frames
 *      sent by the server (CONNECTED, MESSAGE, RECEIPT, ERROR).
 *
 * The two threads share:
 *   - 'connection'  : the TCP socket wrapper (ConnectionHandler)
 *   - 'protocol'    : the StompProtocol object that holds all client state
 *   - 'open'        : flag indicating whether the socket is currently open
 *   - 'is_running'  : flag indicating whether the main loop should keep running
 *
 * FLOW:
 *   main() creates a StompClient and calls process_keyboard_input().
 *   When the user types "login", the TCP connection is opened and the
 *   socket thread is spawned. From that point on, both threads run until
 *   the user types "logout" and the server confirms with a RECEIPT.
 */

#include "StompClient.h"
#include "ConnectionHandler.h"
#include "StompProtocol.h"
#include "StompFrame.h"
#include <iostream>
#include <sstream>

using std::unique_ptr;
using std::thread;
using std::cerr;
using std::getline;
using std::exception;
using std::cin;
using std::endl;
using std::vector;
using std::string;

/*
 * Constructor: initializes the client in a disconnected, idle state.
 *   - connection  = nullptr  : no TCP connection yet
 *   - protocol    = new StompProtocol : the object that processes all STOMP logic
 *   - is_running  = true     : the main keyboard loop starts immediately
 *   - open        = false    : the socket is not yet open
 */
StompClient::StompClient() :
    connection(nullptr),
    protocol(std::unique_ptr<StompProtocol>(new StompProtocol())),
    socket_thread(),
    is_running(true),
    open(false)
    {}

/*
 * Destructor: ensures all resources are cleaned up when the client is destroyed.
 * Calls stop() which signals the threads to finish and closes the socket.
 */
StompClient::~StompClient() {
	stop();
}

/*
 * read_from_socket()
 * ------------------
 * Runs on the SOCKET THREAD. Loops while the socket is open, blocking on each
 * call to getFrameAscii() until a complete STOMP frame arrives (terminated by '\0').
 *
 * Once a frame arrives:
 *   1. The raw string is split into tokens to quickly identify the frame type.
 *   2. The frame is fully parsed into a StompFrame object.
 *   3. The frame is handed to StompProtocol::processReceivedFrame() for handling.
 *
 * Special case for RECEIPT:
 *   After processing the RECEIPT frame, we check if the protocol is still logged in.
 *   If not, it means the RECEIPT was confirming a DISCONNECT (logout), so we close
 *   the socket and stop this thread.
 */
void StompClient::read_from_socket() {

    while (open) {
        string inFrameStr;
        // Block here until a full frame is received from the server.
        // getFrameAscii reads bytes one by one until the '\0' terminator.
        if (!connection -> getFrameAscii(inFrameStr, '\0')) {
            cerr << "Error reading from socket" << endl;
            open = false;
            continue;
        }

        // Peek at the first word of the raw string to identify the command type
        // before doing a full parse. This lets us handle RECEIPT specially.
        vector<string> args;
        istringstream iss(inFrameStr);
        string word;
        while (iss >> word) {
            args.push_back(word);
        }

        try {
            if (args[0] == "RECEIPT") {
                // Parse the full frame and let the protocol handle it.
                StompFrame inFrame = StompFrame::parse(inFrameStr);
                protocol -> processReceivedFrame(inFrame);
                // After processing a RECEIPT, check if the user was logged out.
                // If loggedIn() is false, the RECEIPT was for our DISCONNECT frame,
                // meaning the server confirmed the logout. We now close the socket.
                if (!protocol->loggedIn()) {
                    connection->close();
                    open = false;
                }
            }
            else {
                // For all other frame types (CONNECTED, MESSAGE, ERROR),
                // just parse and delegate to the protocol handler.
                StompFrame inFrame = StompFrame::parse(inFrameStr);
                protocol->processReceivedFrame(inFrame);
            }
        }
        catch (const exception& e) {
            std::cerr << "Error processing received frame: " << e.what() << endl;
        }
    }
}

/*
 * handleLogin()
 * -------------
 * Opens the TCP socket to the server and spawns the socket-reader thread.
 * Called only when a "login" command is received and we are not already connected.
 *
 * Steps:
 *   1. Parse host and port from the "host:port" argument.
 *   2. If there was a previous socket thread (from a prior session), wait for it to finish.
 *   3. Create a new ConnectionHandler (TCP socket wrapper) and connect to the server.
 *   4. Spawn the socket-reader thread to begin listening for server frames.
 */
void StompClient::handleLogin(const vector<string>& args) {
    string host = args[1].substr(0, args[1].find(":"));
    string port = args[1].substr(args[1].find(":") + 1);
    string username = args[2];
    string password = args[3];
    if(!open){
        // If a previous socket thread exists from a prior login session,
        // wait for it to finish cleanly before creating a new one.
        if (socket_thread.joinable()) {
            socket_thread.join();
        }
        connection = unique_ptr<ConnectionHandler>(new ConnectionHandler(host, std::stoi(port)));
        if (!connection->connect()) {
            throw std::runtime_error("Failed to connect to server");
        }
        is_running = true;
        // Start the background thread that reads incoming frames from the server.
        socket_thread = thread(&StompClient::read_from_socket, this);
        open = true;
    }
}

/*
 * process_keyboard_input()
 * ------------------------
 * Runs on the MAIN THREAD. Loops continuously, reading one line at a time from
 * standard input (the terminal). Each line is a user command.
 *
 * The input is split into tokens (args[0] is the command name, the rest are arguments).
 * The command is then dispatched:
 *
 *   "login"   -> Open TCP connection, then send a STOMP CONNECT frame to the server.
 *   "report"  -> Parse a JSON events file; send one STOMP SEND frame per event.
 *   "summary" -> Generate a local text file summary (no network call).
 *   anything else (join, exit, logout) -> Build the appropriate STOMP frame and send it.
 *
 * Note: A StompFrame with an empty command string means the protocol rejected the
 * command (e.g., "join" when not logged in). In that case we skip sending.
 */
void StompClient::process_keyboard_input() {
    string input;
    while (is_running) {
        getline(cin, input);

        // Split the input line into individual words for easier processing.
        vector<string> args;
        istringstream iss(input);
        string word;
        while (iss >> word) {
            args.push_back(word);
        }

        try {
            if (args[0] == "login") {
                // First, let the protocol build the CONNECT frame (validates input, sets username).
                StompFrame frame = protocol -> processKeyboardInput(args);
                if (frame.getCommand() != "") {
                    if(!open){
                        // Open the TCP socket BEFORE sending the CONNECT frame.
                        handleLogin(args);
                        open = true;
                    }
                    // Send the CONNECT frame to the server over the socket.
                    connection->sendFrameAscii(frame.serialize(), '\0');
                } else{
                    continue;
                }
            }
            else if (args[0] == "report") {
                // "report" is handled separately because it produces MULTIPLE frames
                // (one STOMP SEND frame per event in the JSON file), unlike other
                // commands which produce exactly one frame.
                vector<StompFrame> eventsReported = protocol -> report(args[1]);
                if (!eventsReported.empty()) {
                    for (StompFrame frame : eventsReported) {
                        connection->sendFrameAscii(frame.serialize(), '\0');
                    }
                }
            }
            else if (args[0] == "summary") {
                // "summary" is purely local: it reads stored events and writes a text
                // file. No STOMP frame is sent to the server, so we don't call sendFrameAscii.
                protocol -> processKeyboardInput(args);
            }
            else {
                // For join, exit, logout — the protocol builds a single STOMP frame.
                // If the command was invalid (returned empty frame), do not send.
                StompFrame frame = protocol -> processKeyboardInput(args);
                if (frame.getCommand() != "") {
                    connection->sendFrameAscii(frame.serialize(), '\0');
                }
            }
        }
        catch (const exception& e) {
            cerr << "Error processing command: " << e.what() << endl;
        }
    }
}

/*
 * stop()
 * ------
 * Signals both threads to finish and frees network resources.
 * Called by the destructor, so it runs when the StompClient goes out of scope.
 */
void StompClient::stop() {
    is_running = false;  // signal the main loop to stop

    if (socket_thread.joinable()) { // check if the socket thread was ever started
        socket_thread.join();       // wait for it to finish its current iteration
    }

    if (connection) {        // only close if a connection object exists (not nullptr)
        connection->close(); // shut down the TCP socket
    }
}

/*
 * main()
 * ------
 * Program entry point. Expects exactly zero arguments beyond the program name.
 * Creates a single StompClient and starts the keyboard input loop.
 * The program runs until the user logs out or kills the process.
 */
int main(int argc, char *argv[]) {
	if (argc != 1) {
        std::cerr << "enter program name" << std::endl;
        return 1;
    }

    StompClient client;
    client.process_keyboard_input();
	return 0;
}
