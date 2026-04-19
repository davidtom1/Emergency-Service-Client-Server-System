/*
 * event.cpp
 * ---------
 * This file defines the Event class, which represents a single emergency event
 * (e.g., a fire, an ambulance call), and the parseEventsFile() function that
 * reads a JSON file full of events.
 *
 * AN EVENT HAS:
 *   - channel_name      : the topic/channel it belongs to (e.g., "ambulance")
 *   - city              : where the event occurred
 *   - name              : the event's descriptive name
 *   - date_time         : Unix timestamp (seconds since Jan 1, 1970)
 *   - description       : free-text description of what happened
 *   - general_information : key-value map of extra fields (e.g., "active":"true")
 *   - eventOwnerUser    : the username of whoever reported this event
 *
 * TWO CONSTRUCTORS:
 *   1. Full constructor (from individual fields) — used when reading from a JSON file.
 *   2. String constructor (from a plain-text frame body) — used when receiving a
 *      MESSAGE frame from the server. The server sends event data as plain text,
 *      so this constructor parses that text back into structured fields.
 */

#include "../include/event.h"
#include "../include/json.hpp"
#include <iostream>
#include <fstream>
#include <string>
#include <map>
#include <vector>
#include <sstream>
#include <cstring>

using namespace std;
using json = nlohmann::json;

/*
 * Full constructor
 * ----------------
 * Directly initializes all fields from the provided arguments.
 * eventOwnerUser starts empty — it gets set later by setEventOwnerUser()
 * before the event is sent to the server.
 */
Event::Event(std::string channel_name, std::string city, std::string name, int date_time,
             std::string description, std::map<std::string, std::string> general_information)
    : channel_name(channel_name), city(city), name(name),
      date_time(date_time), description(description), general_information(general_information), eventOwnerUser("")
{
}

Event::~Event()
{
}

void Event::setEventOwnerUser(std::string setEventOwnerUser) {
    eventOwnerUser = setEventOwnerUser;
}

const std::string &Event::getEventOwnerUser() const {
    return eventOwnerUser;
}

const std::string &Event::get_channel_name() const
{
    return this->channel_name;
}

const std::string &Event::get_city() const
{
    return this->city;
}

const std::string &Event::get_name() const
{
    return this->name;
}

int Event::get_date_time() const
{
    return this->date_time;
}

const std::map<std::string, std::string> &Event::get_general_information() const
{
    return this->general_information;
}

const std::string &Event::get_description() const
{
    return this->description;
}

/*
 * String constructor — parses a plain-text event report into an Event object.
 * -------------------------------------------------------------------------
 * This constructor is used when the client receives a MESSAGE frame from the
 * server. The body of the MESSAGE frame is a plain-text string with this format:
 *
 *   channel name:<channel>
 *   user:<username>
 *   city: <city>
 *   event name: <name>
 *   date time: <unix timestamp>
 *   general information:
 *     <key>:<value>
 *     <key>:<value>
 *   description:
 *   <description text, may span multiple lines>
 *
 * This is the same format that StompProtocol::report() writes into SEND frame bodies.
 *
 * How parsing works:
 *   - We read the text line by line.
 *   - Each line is split on the first ':' to get a key and value.
 *   - Special keys ("channel name", "city", "event name", etc.) set specific fields.
 *   - Lines after "general information:" and before "description:" are treated as
 *     key-value pairs and stored in general_information_from_string.
 *   - Everything after "description:" is consumed as the description (may be multi-line).
 *   - The inGeneralInformation flag tracks whether we are in the general_information section.
 */
Event::Event(const std::string &frame_body): channel_name(""), city(""),
                                             name(""), date_time(0), description(""), general_information(),
                                             eventOwnerUser("")
{
    stringstream ss(frame_body);
    string line;
    string eventDescription;
    map<string, string> general_information_from_string;
    bool inGeneralInformation = false; // flag: true while reading general_information entries
    while(getline(ss,line,'\n')){
        vector<string> lineArgs;
        if(line.find(':') != string::npos) {
            // Split the line on ':' into at most two parts: key and value
            split_str(line, ':', lineArgs);
            string key = lineArgs.at(0);
            string val;
            if(lineArgs.size() == 2) {
                val = lineArgs.at(1);
            }
            // Match each recognized key and store the value in the correct field
            if(key == "user") {
                eventOwnerUser = val;
            }
            if(key == "channel name") {
                channel_name = val;
            }
            if(key == "city") {
                city = val;
            }
            else if(key == "event name") {
                name = val;
            }
            else if(key == "date time") {
                date_time = std::stoi(val); // convert string timestamp to integer
            }
            else if(key == "general information") {
                // Everything from the next line until "description:" is general_information
                inGeneralInformation = true;
                continue; // do not try to parse this line as a key:value pair
            }
            else if(key == "description") {
                // Everything remaining in the stream is the description (may be multi-line)
                while(getline(ss,line,'\n')) {
                    eventDescription += line + "\n";
                }
                description = eventDescription;
            }

            // If we are in the general_information section, treat this line as a sub-key.
            // Note: the key has a leading space ("  key:value") so we strip it with substr(1).
            if(inGeneralInformation) {
                general_information_from_string[key.substr(1)] = val;
            }
        }
    }
    general_information = general_information_from_string;
}

/*
 * parseEventsFile()
 * -----------------
 * Reads a JSON file from disk and converts it into a names_and_events struct,
 * which holds the channel name and a vector of Event objects.
 *
 * The JSON file is expected to have this structure:
 * {
 *   "channel_name": "ambulance",
 *   "events": [
 *     {
 *       "event_name": "...",
 *       "city": "...",
 *       "date_time": 1234567890,
 *       "description": "...",
 *       "general_information": {
 *         "active": "true",
 *         "forces_arrival_at_scene": "false",
 *         ...
 *       }
 *     },
 *     ...
 *   ]
 * }
 *
 * general_information values can be strings or other JSON types;
 * non-string values are converted to their JSON string representation (via .dump()).
 */
names_and_events parseEventsFile(std::string json_path)
{
    std::ifstream f(json_path);
    json data = json::parse(f);

    std::string channel_name = data["channel_name"];

    // Iterate over each event in the JSON array and build an Event object for each
    std::vector<Event> events;
    for (auto &event : data["events"])
    {
        std::string name = event["event_name"];
        std::string city = event["city"];
        int date_time = event["date_time"];
        std::string description = event["description"];
        std::map<std::string, std::string> general_information;
        for (auto &update : event["general_information"].items())
        {
            // JSON values may be strings, booleans, numbers, etc.
            // We store them all as strings; non-strings are serialized with .dump().
            if (update.value().is_string())
                general_information[update.key()] = update.value();
            else
                general_information[update.key()] = update.value().dump();
        }

        events.push_back(Event(channel_name, city, name, date_time, description, general_information));
    }
    names_and_events events_and_names{channel_name, events};

    return events_and_names;
}

/*
 * split_str()
 * -----------
 * Helper method: splits the input string on the given delimiter character
 * and appends each token to the output vector.
 * Used by the string constructor to split "key:value" lines.
 */
void Event::split_str(const std::string& input, char delimiter, std::vector<std::string>& output) {
    std::stringstream ss(input);
    std::string item;
    while (std::getline(ss, item, delimiter)) {
        output.push_back(item);
    }
}
