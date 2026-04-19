/*
 * ConnectionHandler.cpp
 * ---------------------
 * This file implements the TCP socket layer for the C++ client.
 * It wraps the Boost.Asio networking library in a simple interface.
 *
 * WHAT THIS CLASS DOES:
 *   It handles the raw bytes going in and out of the TCP socket.
 *   It does NOT know anything about the STOMP protocol — it just sends
 *   and receives bytes. The protocol logic lives in StompProtocol.cpp.
 *
 * BLOCKING I/O:
 *   All reads and writes here are BLOCKING — the calling thread waits until
 *   the operation completes (or fails). This is why the client uses a dedicated
 *   socket thread (in StompClient::read_from_socket) that can block on reads
 *   without freezing the keyboard input thread.
 *
 * KEY METHODS:
 *   connect()         -> open the TCP connection to the server
 *   getFrameAscii()   -> read bytes until a delimiter (e.g., '\0') is found
 *   sendFrameAscii()  -> write a string + delimiter to the socket
 *   close()           -> shut down the socket
 */

#include "../include/ConnectionHandler.h"

using boost::asio::ip::tcp;

using std::cin;
using std::cout;
using std::cerr;
using std::endl;
using std::string;

/*
 * Constructor
 * -----------
 * Stores the host and port, and initializes the Boost.Asio I/O service and socket.
 * The socket is not connected yet — connect() must be called separately.
 */
ConnectionHandler::ConnectionHandler(string host, short port) : host_(host), port_(port), io_service_(),
                                                                socket_(io_service_) {}

/*
 * Destructor: ensures the socket is closed when the ConnectionHandler is destroyed.
 */
ConnectionHandler::~ConnectionHandler() {
	close();
}

/*
 * connect()
 * ---------
 * Opens the TCP connection to the server at host_:port_.
 * Returns true on success, false if the connection fails (e.g., server not running).
 *
 * Uses Boost.Asio to resolve the address and connect the socket.
 * If anything goes wrong, the exception is caught and an error is printed.
 */
bool ConnectionHandler::connect() {
	std::cout << "Starting connect to "
	          << host_ << ":" << port_ << std::endl;
	try {
		tcp::endpoint endpoint(boost::asio::ip::address::from_string(host_), port_); // the server endpoint
		boost::system::error_code error;
		socket_.connect(endpoint, error);
		if (error)
			throw boost::system::system_error(error);
	}
	catch (std::exception &e) {
		std::cerr << "Connection failed (Error: " << e.what() << ')' << std::endl;
		return false;
	}
	return true;
}

/*
 * getBytes()
 * ----------
 * Low-level helper: reads exactly 'bytesToRead' bytes from the socket into 'bytes'.
 * BLOCKS until all the requested bytes have been received.
 *
 * Boost.Asio's read_some() may return fewer bytes than requested in one call
 * (this is normal for TCP), so we loop until we have accumulated enough bytes.
 *
 * Returns true on success, false if the connection drops mid-read.
 */
bool ConnectionHandler::getBytes(char bytes[], unsigned int bytesToRead) {
	size_t tmp = 0;
	boost::system::error_code error;
	try {
		while (!error && bytesToRead > tmp) {
			tmp += socket_.read_some(boost::asio::buffer(bytes + tmp, bytesToRead - tmp), error);
		}
		if (error)
			throw boost::system::system_error(error);
	} catch (std::exception &e) {
		std::cerr << "recv failed (Error: " << e.what() << ')' << std::endl;
		return false;
	}
	return true;
}

/*
 * sendBytes()
 * -----------
 * Low-level helper: writes exactly 'bytesToWrite' bytes from 'bytes' to the socket.
 * BLOCKS until all bytes have been sent.
 *
 * Similar to getBytes(), write_some() may send fewer bytes than requested,
 * so we loop until everything has been flushed.
 *
 * Returns true on success, false if the connection is lost while writing.
 */
bool ConnectionHandler::sendBytes(const char bytes[], int bytesToWrite) {
	int tmp = 0;
	boost::system::error_code error;
	try {
		while (!error && bytesToWrite > tmp) {
			tmp += socket_.write_some(boost::asio::buffer(bytes + tmp, bytesToWrite - tmp), error);
		}
		if (error)
			throw boost::system::system_error(error);
	} catch (std::exception &e) {
		std::cerr << "recv failed (Error: " << e.what() << ')' << std::endl;
		return false;
	}
	return true;
}

/*
 * getLine() / sendLine()
 * ----------------------
 * Convenience wrappers: read/send a newline-terminated string.
 * These delegate to getFrameAscii / sendFrameAscii with '\n' as the delimiter.
 * Not currently used by the STOMP client (which uses '\0' as the delimiter),
 * but provided for completeness.
 */
bool ConnectionHandler::getLine(std::string &line) {
	return getFrameAscii(line, '\n');
}

bool ConnectionHandler::sendLine(std::string &line) {
	return sendFrameAscii(line, '\n');
}

/*
 * getFrameAscii()
 * ---------------
 * Reads characters from the socket one byte at a time until the delimiter is found.
 * The delimiter itself is NOT appended to the output string.
 *
 * For STOMP, the delimiter is '\0' (null byte) — every STOMP frame ends with '\0'.
 * This method blocks until a complete frame has arrived.
 *
 * This is the method called by StompClient::read_from_socket() in a loop
 * to receive complete STOMP frames from the server.
 */
bool ConnectionHandler::getFrameAscii(std::string &frame, char delimiter) {
	char ch;
	// Stop when we encounter the null character.
	// Notice that the null character is not appended to the frame string.
	try {
		do {
			if (!getBytes(&ch, 1)) {
				return false;
			}
			if (ch != '\0')
				frame.append(1, ch);
		} while (delimiter != ch);
	} catch (std::exception &e) {
		std::cerr << "recv failed2 (Error: " << e.what() << ')' << std::endl;
		return false;
	}
	return true;
}

/*
 * sendFrameAscii()
 * ----------------
 * Sends the given string to the socket, followed by the delimiter character.
 *
 * For STOMP, the delimiter is '\0'. This method:
 *   1. Sends all the bytes of the frame string.
 *   2. Sends the delimiter byte as the frame terminator.
 *
 * This is called by StompClient to send serialized STOMP frames to the server.
 */
bool ConnectionHandler::sendFrameAscii(const std::string &frame, char delimiter) {
	bool result = sendBytes(frame.c_str(), frame.length());
	if (!result) return false;
	return sendBytes(&delimiter, 1);
}

/*
 * close()
 * -------
 * Closes the TCP socket, releasing the network connection.
 * If the socket is already closed (e.g., server disconnected), the exception is
 * caught and a message is printed rather than crashing.
 */
void ConnectionHandler::close() {
	try {
		socket_.close();
	} catch (...) {
		std::cout << "closing failed: connection already closed" << std::endl;
	}
}
