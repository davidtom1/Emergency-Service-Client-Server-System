/*
 * StompFrameAbstract.java
 * -----------------------
 * Base class for all STOMP frames that the SERVER sends to clients.
 *
 * WHAT IS A STOMP FRAME?
 * A STOMP frame is the unit of communication in the STOMP protocol.
 * Every message has:
 *   - A command  (e.g., "CONNECTED", "MESSAGE", "RECEIPT", "ERROR")
 *   - Headers    (key:value pairs that provide metadata)
 *   - A body     (optional free-text payload)
 *
 * WHY AN ABSTRACT BASE CLASS?
 * All server-to-client frames share the same wire format, so the
 * serialization logic (toString / encode) lives here once.
 * Each concrete subclass (ConnectedFrame, MessageFrame, ErrorFrame, etc.)
 * just populates the command and headers appropriate for that frame type.
 *
 * WIRE FORMAT (produced by toString()):
 *   COMMAND\n
 *   header1:value1\n
 *   header2:value2\n
 *   \n
 *   body\n
 *   \n\0
 */
package bgu.spl.net.impl.stomp.Frame;

import java.util.HashMap;

public abstract class StompFrameAbstract {
    protected String command;                    // e.g., "CONNECTED", "MESSAGE", "ERROR"
    protected HashMap<String, String> headers;   // metadata key-value pairs for this frame type
    protected String body;                       // optional message content (empty string if none)

    /*
     * Constructor
     * -----------
     * Sets the command and initializes empty headers and body.
     * Subclasses call super(command) and then populate headers via this.headers.put().
     */
    protected StompFrameAbstract(String inCommand){
        this.command = inCommand;
        this.headers = new HashMap<>();
        this.body = "";
    }

    public String getCommand() {
        return command;
    }

    public HashMap<String, String> getHeaders() {
        return headers;
    }

    public String getBody() {
        return body;
    }

    public void setBody(String inBody) {
        this.body = inBody;
    }

    /*
     * toString()
     * ----------
     * Serializes this frame to the STOMP wire format string.
     * This is used by encode() to produce the bytes sent to the client.
     *
     * Format:
     *   COMMAND\n
     *   key1:value1\n
     *   key2:value2\n
     *   \n               <- blank line separates headers from body
     *   body\n           <- only written if body is non-empty
     *   \n\0             <- extra newline + null byte terminates the frame
     */
    public String toString() {
        StringBuilder ans = new StringBuilder();
        ans.append(this.command + "\n");
        for (String key : this.headers.keySet()) {
            String value = this.headers.get(key);
            ans.append(key + ":" + value + "\n");
        }
        ans.append("\n"); // Empty line between headers and body (required by STOMP spec)
        if (body != "") {
            ans.append(body + "\n");
        }
        ans.append("\n" + '\u0000'); // Null character (\0) terminates the frame
        return ans.toString();
    }

    /*
     * encode()
     * --------
     * Converts the frame to a byte array ready to be written to the socket.
     * Calls toString() and converts the resulting string to bytes.
     */
    public byte[] encode() {
        return toString().getBytes();
    }

}
