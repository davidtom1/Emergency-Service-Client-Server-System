/*
 * StompFrame.cpp
 * --------------
 * This file defines the StompFrame class, which represents a single STOMP protocol message.
 *
 * WHAT IS A STOMP FRAME?
 * A STOMP frame is the unit of communication in the STOMP protocol.
 * Every message sent or received — whether a CONNECT, SUBSCRIBE, MESSAGE, etc. —
 * is a frame. A frame always has this structure on the wire:
 *
 *   COMMAND\n
 *   header1:value1\n
 *   header2:value2\n
 *   \n                  <- blank line separates headers from body
 *   body text here
 *   \0                  <- null byte (\u0000) terminates the frame
 *
 * This class provides:
 *   - Constructors to build a frame from parts
 *   - parse()     : convert a raw string into a StompFrame object
 *   - serialize() : convert a StompFrame object back into a raw string for sending
 *   - Getters for command, headers, and body
 */

#include "../include/StompFrame.h"
#include <sstream>
#include <string>
#include <iostream>
using std::istringstream;
using std::stringstream;
using namespace std;
using std::cout;

// Default constructor: creates an empty frame with no command, headers, or body.
// An empty command ("") is used as a sentinel to signal "no frame to send".
StompFrame::StompFrame() : command(""), headers(), body("") {}

// Parameterized constructor: builds a frame from its three components.
StompFrame::StompFrame(string cmd, map<string, string> hdrs, string frameBody) :
    command(cmd), headers(hdrs), body(frameBody) {}


/*
 * parse()
 * -------
 * Static factory method: parses a raw STOMP frame string into a StompFrame object.
 *
 * The raw string comes from the network (read until the '\0' terminator).
 * It has the structure:
 *
 *   COMMAND\n
 *   key1:value1\n
 *   key2:value2\n
 *   \n
 *   body text\n
 *   \0    <- this byte has already been stripped by getFrameAscii before calling here
 *
 * Parsing steps:
 *   1. Read the first line  -> that is the command (e.g., "CONNECTED", "MESSAGE")
 *   2. Read lines until an empty line -> those are headers (key:value pairs)
 *   3. Read remaining lines -> that is the body (stops at null byte or end of string)
 */
StompFrame StompFrame::parse(const string& stringFrame) {
    istringstream stream(stringFrame);
    string line;
    string command;
    map<string, string> headers;
    string body;

    // Step 1: The first line is always the command name
    getline(stream, command);

    // Step 2: Read header lines until we hit an empty line.
    // Each header line is formatted as "key:value" (colon-separated).
    // Leading spaces after the colon are stripped.
    while (getline(stream, line) && !line.empty()) {
        size_t colonIndex = line.find(":");
        if (colonIndex != string::npos) {
            string key = line.substr(0, colonIndex);
            string value = line.substr(colonIndex + 1);
            value.erase(0, value.find_first_not_of(" ")); // strip leading space after colon
            headers[key] = value;
        }
    }

    // Step 3: Everything after the blank line is the body.
    // We stop if we encounter a null byte (the frame terminator).
    string bodyLine;
    while (getline(stream, bodyLine)) {
        if (bodyLine[0] == '\0') break;
        body += bodyLine + "\n";
    }

    return StompFrame(command, headers, body);
}

/*
 * serialize()
 * -----------
 * Converts this StompFrame into the raw wire format string that can be sent
 * over the TCP socket. The output exactly follows the STOMP frame structure:
 *
 *   COMMAND\n
 *   header1:value1\n
 *   header2:value2\n
 *   \n              <- blank line between headers and body
 *   body
 *   \0              <- null byte frame terminator
 *
 * This is the inverse of parse().
 */
string StompFrame::serialize() const {
    stringstream ss;
    ss << command << '\n';

    for (const auto& header : headers) {
        ss << header.first << ":" << header.second << "\n";
    }

    ss << "\n"; // blank line separates headers from body (required by STOMP spec)
    ss << body;
    ss << "\0"; // STOMP frame terminator — must be present for the receiver to know the frame ended

    string result = ss.str();
    return result;
}

// Returns the command string (e.g., "CONNECT", "MESSAGE", "DISCONNECT").
string StompFrame::getCommand() const {return command;}

// Returns the full headers map.
map<string, string> StompFrame::getHeaders() const {return headers;}

// Returns the body string.
string StompFrame::getBody() const {return body;}

/*
 * getHeader()
 * -----------
 * Looks up a single header value by its key name.
 * Returns an empty string if the key does not exist in the headers map.
 * This is safer than using headers.at(key) directly, which would throw if missing.
 */
string StompFrame::getHeader(const string& key) const {
    auto it = headers.find(key); // search the map for the key
    // If found (iterator not at end), return the value; otherwise return empty string.
    return (it != headers.end()) ? it->second : "";
}
