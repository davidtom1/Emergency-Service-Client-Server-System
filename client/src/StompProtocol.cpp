/*
 * StompProtocol.cpp
 * -----------------
 * This file contains all the STOMP protocol logic for the CLIENT side.
 *
 * It is responsible for two things:
 *   1. OUTGOING: Translating user keyboard commands into STOMP frames to be sent.
 *   2. INCOMING: Handling STOMP frames received from the server.
 *
 * STATE MANAGED HERE:
 *   - Whether the user is currently logged in (isLoggedIn)
 *   - Which channels the user is subscribed to (myChannels, channelSubs)
 *   - All events received from the server (events vector)
 *   - Subscription ID counter (nextSubsctiptionId) — each SUBSCRIBE needs a unique ID
 *   - Receipt ID counter (nextReciptId) — DISCONNECT requires a receipt ID
 *   - logoutId — the receipt ID we're waiting for to confirm logout
 *
 * HOW STOMP WORKS (brief):
 *   The client sends "command frames" to the server:
 *     CONNECT      -> log in
 *     SUBSCRIBE    -> start receiving messages from a channel
 *     UNSUBSCRIBE  -> stop receiving messages from a channel
 *     SEND         -> publish a message to a channel
 *     DISCONNECT   -> log out (with a receipt so we know the server got it)
 *
 *   The server sends "response frames" back:
 *     CONNECTED    -> login was accepted
 *     MESSAGE      -> someone published to a channel we're subscribed to
 *     RECEIPT      -> server confirms it processed our DISCONNECT
 *     ERROR        -> something went wrong
 */

#include "../include/StompProtocol.h"
#include "../include/event.h"
#include "../include/StompFrame.h"
#include <iostream>
#include <string>
#include <atomic>
#include <vector>
#include <set>
#include <fstream>
#include <algorithm>
#include <ctime>
using namespace std;
using std::cout;

/*
 * keyCommand enum + keyStringToCommand()
 * ---------------------------------------
 * Maps the string the user types (e.g., "join") to an enum value so we can use
 * a switch statement instead of a chain of if/else-if comparisons.
 * "failed" is the fallback for unrecognized commands.
 */
enum keyCommand {
    login,
    join,
    exitChannel,
    logout,
    summary,
    failed,
};
keyCommand keyStringToCommand(const std::string& command) {
    if (command == "login") return login;
    if (command == "join") return join;
    if (command == "exit") return exitChannel;
    if (command == "logout") return logout;
    if (command == "summary") return summary;
    return failed;
}

/*
 * Constructor
 * -----------
 * Initializes a fresh, disconnected protocol state.
 *   - connectionId     : unique ID for this client instance (auto-incremented)
 *   - username         : empty until login
 *   - isLoggedIn       : false until the server sends CONNECTED
 *   - nextSubsctiptionId : starts at 1, incremented each time we subscribe
 *   - nextReciptId     : starts at 1, used to tag our DISCONNECT frame
 *   - logoutId         : -1 means we have not sent a DISCONNECT yet
 *   - channelSubs      : maps channel name -> subscription ID (needed to unsubscribe)
 *   - myChannels       : list of channel names we are currently subscribed to
 *   - events           : all MESSAGE frames received, stored as Event objects
 */
StompProtocol::StompProtocol():
connectionId(connectionId++),username(),isLoggedIn(false),
nextSubsctiptionId(1),nextReciptId(1),
logoutId(-1), // -1 means no pending logout
channelSubs(),myChannels(),events()
{}

/*
 * processKeyboardInput()
 * ----------------------
 * The main dispatcher for OUTGOING commands (user -> server).
 * Takes the tokenized user input as 'args', where args[0] is the command name.
 *
 * Validates argument counts and delegates to the appropriate handler.
 * Returns a StompFrame ready to be serialized and sent.
 * Returns an empty StompFrame (command == "") when the command should not be sent
 * (e.g., validation failed, or "summary" which produces no network frame).
 */
StompFrame StompProtocol::processKeyboardInput(const std::vector<std::string>& args){
        string frameType= args[0];
        switch (keyStringToCommand(frameType)) {
            case keyCommand::login:
                if(args.size()==4){
                    return login(args[1],args[2],args[3]);
                }
                else{
                    cout << "login command needs 3 args: {host:port} {username} {password}"<<"\n";
                    return StompFrame();
                }
                break;
            case keyCommand::join:
                if(args.size()==2){
                   return join(args[1]);
                }
                else{
                    cout << "join command needs 1 args: {channel_name}"<<"\n";
                    return StompFrame();
                }
                break;
            case keyCommand::exitChannel:
                if(args.size()==2){
                    return exit(args[1]);
                }
                else{
                    cout << "exit command needs 1 args: {channel_name}"<<"\n";
                    return StompFrame();
                }
                break;

            case keyCommand::logout:
                return logout();
                break;
            case keyCommand::summary:
                if(args.size()==4){
                    summary(args[1], args[2], args[3]);
                }
                else{
                    cout << "summary command needs 3 args: {channel_name} {user} {file}"<<"\n";
                    return StompFrame();
                }
                break;
            default:
                cout << "invalid command"<<"\n";
                return StompFrame();
        }
        return StompFrame();

}

/*
 * login()
 * -------
 * Handles the "login" command. Builds a STOMP CONNECT frame to send to the server.
 *
 * The CONNECT frame tells the server:
 *   - accept-version: 1.2  -> which STOMP version we support
 *   - host              -> the virtual host name (fixed to the course server address)
 *   - login             -> the username
 *   - passcode          -> the password
 *
 * Does NOT actually open the TCP socket — that is done by StompClient::handleLogin().
 * This method only builds the frame. If the user is already logged in, we reject.
 */
StompFrame StompProtocol::login(string hostPort, string user, string password){
    size_t i= hostPort.find(":");
    string host= hostPort.substr(0,i);
    if(isLoggedIn==true){
        cout << "The client is already logged in, log out before trying again" <<"\n";
        return StompFrame();
    }
    else{
        username=user;
        // Build CONNECT frame with STOMP 1.2 required headers
        map<string, string> hdrs;
        hdrs.insert({"accept-version","1.2"});
        hdrs.insert({"host","stomp.cs.bgu.ac.il"});
        hdrs.insert({"login",username});
        hdrs.insert({"passcode", password});
        StompFrame frame("CONNECT", hdrs, " ");
        return frame;
    }
}

/*
 * join()
 * ------
 * Handles the "join <channel>" command. Builds a STOMP SUBSCRIBE frame.
 *
 * The SUBSCRIBE frame tells the server to start forwarding messages from
 * the given channel to this client.
 *
 * Each subscription gets a unique integer ID (nextSubsctiptionId).
 * This ID is stored in channelSubs so we can look it up later when
 * we want to unsubscribe (the UNSUBSCRIBE frame requires the ID, not the name).
 *
 * The local state (myChannels and channelSubs) is updated immediately,
 * before the server even replies. This is consistent with STOMP semantics
 * where SUBSCRIBE does not require a receipt.
 */
StompFrame StompProtocol::join(string channel){
    if(isLoggedIn==false){ // guard: must be logged in first
        cout << "please login first" <<"\n";
        return StompFrame();
    }

    if(isSubscribed(channel)){  // guard: cannot subscribe to the same channel twice
        cout << "cannot subscribe to a channel you are already subscribed to" <<"\n";
        return StompFrame();
    }
    map<string, string> hdrs;
    hdrs.insert({"destination",channel});
    hdrs.insert({"id",to_string(nextSubsctiptionId)});
    StompFrame frame("SUBSCRIBE", hdrs, "\n");
    myChannels.push_back(channel);                       // remember channel name for isSubscribed checks
    channelSubs.insert({channel, nextSubsctiptionId});   // remember subId so we can unsubscribe later
    nextSubsctiptionId++;                                // increment for the next subscription
    cout << "Joined channel " + channel <<"\n";
    return frame;
}

/*
 * exit()
 * ------
 * Handles the "exit <channel>" command. Builds a STOMP UNSUBSCRIBE frame.
 *
 * The UNSUBSCRIBE frame requires the numeric subscription ID, NOT the channel name.
 * So we look up the ID from channelSubs using the channel name, then remove
 * the channel from both local tracking structures.
 */
StompFrame StompProtocol::exit(string channel){
    if(isLoggedIn==false){ // guard: must be logged in first
        cout << "please login first"<<"\n";
        return StompFrame();
    }
    if(!isSubscribed(channel)){ // guard: cannot unsubscribe from a channel we never joined
        cout << "you are not subscribed to the channel " + channel<<"\n";
        return StompFrame();
    }
    // Look up the numeric subscription ID for this channel name.
    // The STOMP protocol requires UNSUBSCRIBE to use the same ID as the original SUBSCRIBE.
    int subId = channelSubs.find(channel)->second;
    map<string, string> hdrs;
    hdrs.insert({"id",to_string(subId)});
    StompFrame frame("UNSUBSCRIBE", hdrs, "\n");
    // Remove channel from both local tracking structures
    myChannels.erase(std::remove(myChannels.begin(), myChannels.end(), channel), myChannels.end());
    channelSubs.erase(channel);
    cout << "Exited channel " << channel << "\n";
    return frame;

}

/*
 * report()
 * --------
 * Handles the "report <json-file>" command.
 *
 * Reads a JSON file containing a list of emergency events for a specific channel.
 * For EACH event, builds a STOMP SEND frame with the event details in the body.
 * Returns a vector of frames — the caller (StompClient) sends them all one by one.
 *
 * The body of each SEND frame is plain text in the format:
 *   user:<username>
 *   city: <city>
 *   event name: <name>
 *   date time: <unix timestamp>
 *   general information:
 *     <key>:<value>
 *     ...
 *   description:
 *   <description text>
 *
 * This text format is what the server broadcasts to other subscribers as a MESSAGE.
 * When the client receives a MESSAGE, it uses the Event(string) constructor to
 * parse this exact format back into an Event object.
 */
vector<StompFrame> StompProtocol::report(string json_path){
    if(isLoggedIn==false){ // guard: must be logged in first
        cout << "please login first"<<"\n";
        return vector<StompFrame>();
    }
    names_and_events eventsTxt= parseEventsFile(json_path); // parse the JSON file into Event objects
    string channel = eventsTxt.channel_name;
    if(!isSubscribed(channel)) { // guard: must be subscribed to the channel to report events on it
        cout << "you are not subscribed to channel " + channel<<"\n";
        return vector<StompFrame>();
    }
    vector<StompFrame> report= vector<StompFrame>();
    for(Event& event : eventsTxt.events) {
        event.setEventOwnerUser(username); // tag this event with the sending user's name
        map<string, string> hdrs = {{"destination", channel}}; // the channel is the STOMP destination
        // Build the general_information section of the body (key:value pairs, indented with 2 spaces)
        string generalInfo;
        const auto& generalInfoMap = event.get_general_information();
        for(auto it = generalInfoMap.begin(); it != generalInfoMap.end(); ++it) {
            generalInfo += "  " + it->first + ":" + it->second + "\n";
        }
        // Assemble the full body of the SEND frame as a plain-text report
        string body = "user:" + username + "\n" +
                        "city: " + event.get_city() + "\n" +
                        "event name: " + event.get_name() + "\n" +
                        "date time: " + std::to_string(event.get_date_time()) + "\n" +
                        "general information:" + "\n" + generalInfo +
                        "description:" + "\n" + event.get_description();
        string command = "SEND";
        StompFrame frame(command, hdrs, body);
        report.push_back(frame);
    }
        cout << "reported"<<"\n";
        return report;

}

/*
 * logout()
 * --------
 * Handles the "logout" command. Builds a STOMP DISCONNECT frame.
 *
 * The DISCONNECT frame includes a "receipt" header with a unique ID.
 * The server will reply with a RECEIPT frame containing that same ID.
 * When we receive the RECEIPT, we know the server processed our DISCONNECT
 * and it is safe to close the socket.
 *
 * We store the receipt ID in logoutId so that reciptFrame() can detect
 * which RECEIPT corresponds to our logout (in case other receipts arrive).
 */
StompFrame StompProtocol::logout(){
    if(isLoggedIn==false){ // guard: must be logged in first
        cout << "please login first"<<"\n";
        return StompFrame();
    }
    else{
    string command = "DISCONNECT";
    string body= "";
    map<string, string> hdrs ;
    hdrs.insert({"receipt",to_string(nextReciptId)});
    StompFrame frame(command, hdrs ,body);
    logoutId=nextReciptId; // save which receipt ID will confirm this logout
    nextReciptId++;        // increment for any future receipts
    return frame;
    }
}

/*
 * summary()
 * ---------
 * Handles the "summary <channel> <user> <output-file>" command.
 *
 * This is a PURELY LOCAL operation — no STOMP frame is sent.
 * It reads from the 'events' vector (which is filled as MESSAGE frames arrive)
 * and writes a formatted text file summarizing all events for the given user
 * on the given channel.
 *
 * The output file format:
 *   Channel <channel>
 *   Stats:
 *   Total: <count>
 *   Active: <count of events where active==true>
 *   Forced Arrival: <count of events where forces_arrival_at_scene==true>
 *   Report_1:
 *     city: ...
 *     date time: ...  (human-readable, converted from unix timestamp)
 *     event name: ...
 *     summary: ...    (truncated to 27 characters if longer)
 *   Report_2:
 *     ...
 *
 * Two passes are made over the events vector:
 *   Pass 1: Count totals (total events, active, forced arrival)
 *   Pass 2: Write individual event details
 *
 * Events are already sorted by date (then name) by messageFrame() as they arrive,
 * so the output is in chronological order without any sorting here.
 */
void StompProtocol::summary(string channel, string user ,string txtName){
    if(isLoggedIn == false) { // guard: must be logged in first
        cout << "please login first" << "\n";
        return;
    }
    if (!isSubscribed(channel)) {
        cout << "You are not subscribed to channel " + channel << "\n";
        return;
    }

    std::ofstream outFile(txtName);
    outFile << "Channel " << channel <<"\n";
    outFile << "Stats:" << "\n";
    int total = 0;
    int active = 0;
    int forces = 0;

    // First pass: count aggregate statistics for this user/channel
    for (const Event& event : events) {
        if (event.getEventOwnerUser() == user && event.get_channel_name()== channel) {
            total++;
            const auto& info = event.get_general_information();

            if (info.find(" active") != info.end()) {  // Use find instead of at
                if (info.find(" active")->second == "true")
                    active++;
            }
            if (info.find(" forces_arrival_at_scene") != info.end()) {
                if (info.find(" forces_arrival_at_scene")->second == "true")
                    forces++;
            }

        }
    }
    outFile << "Total: " << total << "\n"
    << "Active: " << active << "\n"
    << "Forced Arrival: " << forces << "\n";
    total=0;
    // Second pass: write individual event entries.
    // Descriptions longer than 27 characters are truncated to 26 chars + "..."
    for (const Event& event : events) {
        if (event.getEventOwnerUser() == user && event.get_channel_name()== channel) {
            total++;
            outFile << "Report_" << total << ":" << "\n"
                 << "  city: " << event.get_city() << "\n"
                 << "  date time: " << epoch_to_date(event.get_date_time()) << "\n"
                 << "  event name: " << event.get_name() << "\n";
                 string s = event.get_description();
                 if (s.size() <= 27 ){
                    outFile << "  summary: " << event.get_description();
                 }
                 else{
                    outFile << "  summary: " << event.get_description().substr(0, 26) << "..." << "\n";
                 }
        }
    }
    outFile.close();
    cout << "Summary written to: " << txtName <<"\n";

}

/*
 * isSubscribed()
 * --------------
 * Helper: returns true if we are currently subscribed to the given channel.
 * Used as a guard in join(), exit(), report(), and summary().
 */
bool StompProtocol::isSubscribed(string channel){
    for(string c: myChannels){
        if(c== channel)
            return true;
    }
    return false;
}

// ─── INCOMING FRAME HANDLERS ────────────────────────────────────────────────
// These are called by the SOCKET THREAD when the server sends us a frame.

/*
 * serverCommand enum + serverStringToCommand()
 * ---------------------------------------------
 * Same pattern as keyCommand above: maps the frame's command string to an enum
 * so we can dispatch it with a switch statement in processReceivedFrame().
 */
enum serverCommand {
    CONNECTED,
    MESSAGE,
    RECEIPT,
    ERROR,
    DEFAULT,
};
serverCommand serverStringToCommand(const std::string& command) {
    if (command == "CONNECTED") return CONNECTED;
    if (command == "MESSAGE") return MESSAGE;
    if (command == "RECEIPT") return RECEIPT;
    if (command == "ERROR") return ERROR;
    return DEFAULT;


}

/*
 * processReceivedFrame()
 * ----------------------
 * Main dispatcher for INCOMING frames from the server.
 * Reads the command field of the frame and delegates to the right handler.
 */
void StompProtocol::processReceivedFrame(const StompFrame& frame){
        string frameType = frame.getCommand();
        switch (serverStringToCommand(frameType)) {
            case serverCommand::CONNECTED:
                connectedFrame(frame);
                break;
            case serverCommand::MESSAGE:
                    messageFrame(frame);
                break;
            case serverCommand::RECEIPT:
                    reciptFrame(frame);
                break;
            case serverCommand::ERROR:
                    errorFrame(frame);
                    break;
            default:
                break;

    }
}

/*
 * connectedFrame()
 * ----------------
 * Called when the server sends a CONNECTED frame, which means our CONNECT
 * (login) was accepted. We flip isLoggedIn to true and print confirmation.
 */
void StompProtocol::connectedFrame(const StompFrame& frame){
    isLoggedIn=true;
    cout << "Login succesful" << "\n";
}

/*
 * messageFrame()
 * --------------
 * Called when the server broadcasts a MESSAGE frame to this client.
 * This happens when another user (or this user) sent a SEND frame to
 * a channel that this client is subscribed to.
 *
 * The frame body contains a plain-text event report. We parse it into
 * an Event object using the Event(string) constructor, then add it to
 * the events vector.
 *
 * After adding, the vector is re-sorted by date_time (oldest first),
 * with alphabetical event name as the tiebreaker. This keeps events
 * in a consistent order regardless of the order they were received,
 * which is important for deterministic summary output.
 */
void StompProtocol::messageFrame(const StompFrame& frame){
    string channel = frame.getHeader("destination");
    if (!channel.empty()) {
        // The Event(string) constructor needs "channel name:<channel>" as the first line.
        // The server doesn't include it in the MESSAGE body, so we prepend it here.
        string report = "channel name:" + channel + "\n";
        report = report + frame.getBody();
        Event event(report);
        events.emplace_back(event);
        // Sort events by date (ascending), with name as tiebreaker for determinism.
        sort(events.begin(), events.end(), [](const Event& e1, const Event& e2) {
            if (e1.get_date_time() == e2.get_date_time()) {
                return e1.get_name() < e2.get_name();
            }
            return e1.get_date_time() < e2.get_date_time();
        });
    }
}

/*
 * reciptFrame()
 * -------------
 * Called when the server sends a RECEIPT frame.
 * A RECEIPT is only sent in response to a DISCONNECT frame.
 *
 * We compare the receipt-id in the frame against logoutId (the ID we
 * stored when we sent DISCONNECT). If they match, the logout is confirmed:
 *   - Mark the user as logged out
 *   - Clear all subscription and event state (ready for a future re-login)
 *
 * After this returns, StompClient::read_from_socket() detects that
 * loggedIn() is now false and closes the socket.
 */
void StompProtocol::reciptFrame(const StompFrame& frame){
    string recId= frame.getHeader("receipt-id");
    if(recId != ""){
        int Id = std::stoi(recId);
        // Check if this receipt matches the ID from our DISCONNECT frame
        if(Id == logoutId){
            isLoggedIn=false;
            // Reset all state so the client is ready for a fresh login session
            myChannels.clear();
            nextSubsctiptionId=1;
            nextReciptId=1;
            channelSubs.clear();
            events.clear();
            cout << "logged out" << "\n";
        }
    }
}

/*
 * errorFrame()
 * ------------
 * Called when the server sends an ERROR frame.
 * Prints the error message (from the "message" header) and the body
 * (which contains the original frame that caused the error).
 */
void StompProtocol::errorFrame(const StompFrame& frame){
    string error = frame.getHeader("message");
    if(error != ""){
        string body= frame.getBody();
        string print= error + "\n" + body;
        cout << print;
    }


}

/*
 * loggedIn()
 * ----------
 * Returns true if the user is currently authenticated with the server.
 * Used by StompClient::read_from_socket() to detect when logout is complete.
 */
bool StompProtocol::loggedIn() const {
    return isLoggedIn;
}

/*
 * epoch_to_date()
 * ---------------
 * Converts a Unix timestamp (seconds since Jan 1, 1970) into a human-readable
 * date string in the format "DD/MM/YYYY HH:MM".
 * Used when writing the summary file so dates are readable rather than raw numbers.
 */
string StompProtocol::epoch_to_date(int timestamp) {
    std::time_t time = timestamp;
    std::tm* localTime = std::localtime(&time);
    char buffer[20];
    std::strftime(buffer, sizeof(buffer), "%d/%m/%Y %H:%M", localTime);
    return buffer;
}
