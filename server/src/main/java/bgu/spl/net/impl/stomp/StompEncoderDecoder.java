/*
 * StompEncoderDecoder.java
 * ------------------------
 * This class handles the RAW BYTE LAYER of the STOMP protocol on the server.
 * It sits between the TCP socket and the StompProtocol logic.
 *
 * It has two responsibilities:
 *
 *   1. DECODING (bytes -> frame object):
 *      The server receives bytes from a client one at a time.
 *      decodeNextByte() accumulates bytes until it sees the '\0' terminator,
 *      then parses the accumulated bytes into a StompFrameAbstract object.
 *
 *   2. ENCODING (frame object -> bytes):
 *      When the server needs to send a frame to a client,
 *      encode() converts the StompFrameAbstract object into a byte array
 *      by calling toString() on the frame (which produces the STOMP wire format).
 *
 * THERE IS ONE INSTANCE PER CLIENT — each client connection gets its own
 * StompEncoderDecoder (created by the factory in StompServer).
 *
 * HOW DECODING WORKS (byte by byte):
 *   The TCP stream is a continuous flow of bytes. STOMP frames are delimited
 *   by the null byte '\0' (Unicode '\u0000'). We accumulate bytes into a
 *   ByteBuffer until '\0' arrives, at which point we know one complete frame
 *   has been received and can be parsed.
 */
package bgu.spl.net.impl.stomp;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.ConcurrentHashMap;

import bgu.spl.net.api.MessageEncoderDecoder;
import bgu.spl.net.impl.stomp.Frame.ConnectFrame;
import bgu.spl.net.impl.stomp.Frame.DisconnectFrame;
import bgu.spl.net.impl.stomp.Frame.SendFrame;
import bgu.spl.net.impl.stomp.Frame.StompFrameAbstract;
import bgu.spl.net.impl.stomp.Frame.SubscribeFrame;
import bgu.spl.net.impl.stomp.Frame.UnsubscribeFrame;

public class StompEncoderDecoder implements MessageEncoderDecoder<StompFrameAbstract> {

    // Accumulates incoming bytes until a complete frame has arrived.
    // 1024 bytes is the initial capacity; Boost.Asio may deliver bytes in small chunks.
    private ByteBuffer buffer = ByteBuffer.allocate(1024);

    /*
     * decodeNextByte()
     * ----------------
     * Called once for each byte received from the client.
     * Adds the byte to the buffer.
     *
     * Returns null while the frame is still incomplete (more bytes expected).
     * Returns a StompFrameAbstract once the '\0' terminator arrives,
     * signaling that a complete frame has been assembled.
     *
     * After decoding, the buffer is cleared so it's ready for the next frame.
     */
    @Override
    public StompFrameAbstract decodeNextByte(byte nextByte) {
        buffer.put(nextByte);
        if (nextByte == '\u0000') { // '\0' marks the end of a STOMP frame
            buffer.flip();          // switch buffer from write mode to read mode
            StompFrameAbstract frame = decodeFrame(buffer);
            buffer.clear();         // reset for the next frame
            return frame;
        }
        return null; // frame not yet complete — keep accumulating bytes
    }

    /*
     * decodeFrame()
     * -------------
     * Converts the bytes in the buffer into a StompFrameAbstract object.
     *
     * The buffer contains the raw STOMP frame text (UTF-8 encoded):
     *   COMMAND\n
     *   header1:value1\n
     *   header2:value2\n
     *   \n
     *   body
     *   \0
     *
     * Steps:
     *   1. Convert buffer bytes to a UTF-8 string and split on newlines.
     *   2. The first element is the command.
     *   3. Parse headers (lines between the command and the first empty line).
     *   4. Parse the body (everything after the empty line).
     *   5. Create the appropriate subclass of StompFrameAbstract based on the command.
     */
    private StompFrameAbstract decodeFrame(ByteBuffer buffer) {
        byte[] bytes = new byte[buffer.remaining()];
        buffer.get(bytes); // copy buffer contents into the array
        // Convert the raw bytes to a String, trim whitespace/null bytes
        String frameString = new String(bytes, StandardCharsets.UTF_8).trim();
        String[] lines = frameString.split("\n"); // split into lines for parsing
        if (lines.length < 2) {
            throw new IllegalArgumentException("invalid stomp frame");
        }

        String command = lines[0]; // first line is always the STOMP command
        ConcurrentHashMap<String, String> headers = parseHeaders(lines);
        String body = parseBody(lines);
        StompFrameAbstract frame = createFrame(command, headers, body);
        return frame;

    }

    /*
     * parseHeaders()
     * --------------
     * Extracts the headers from the lines array (lines[1] through the first empty line).
     * Each header line has the format "key:value".
     * Stops when an empty line is found (that empty line separates headers from body).
     * Returns a ConcurrentHashMap of header key -> value pairs.
     */
    private ConcurrentHashMap<String, String> parseHeaders(String[] lines) {
        ConcurrentHashMap<String, String> ansHeaders = new ConcurrentHashMap<>();
        for (int i = 1; i < lines.length; i++) {
            String line = lines[i].trim(); // strip any extra whitespace
            if (line.isEmpty()) { // empty line signals end of headers
                break;
            }
            int colonIndex = line.indexOf(':'); // headers are "key:value" separated by the first colon
            if (colonIndex != -1) {
                String headerName = line.substring(0, colonIndex).trim();
                String headerVal = line.substring(colonIndex + 1).trim();
                ansHeaders.put(headerName, headerVal);
            }
        }
        return ansHeaders;
    }

    /*
     * parseBody()
     * -----------
     * Extracts the body from the lines array.
     * The body starts after the first empty line that follows the headers.
     * If there is no empty line or nothing after it, the body is empty string.
     *
     * Returns the body as a single string (with newlines joining the body lines).
     */
    private String parseBody(String[] lines) {
        StringBuilder bodybuild = new StringBuilder();
        int bodyStartIndex = -1;
        // Find the empty line that separates headers from body
        for (int i = 1; i < lines.length; i++) {
            if (lines[i].trim().isEmpty()) {
                bodyStartIndex = i + 1; // body starts on the line AFTER the empty line
                break;
            }
        }

        if (bodyStartIndex == -1 || bodyStartIndex >= lines.length) {
            return ""; // no body found
        }

        // Concatenate all body lines back into a single string
        for (int i = bodyStartIndex; i < lines.length; i++) {
            bodybuild.append(lines[i]).append("\n");
        }
        return bodybuild.toString();
    }

    /*
     * createFrame()
     * -------------
     * Factory method: creates the correct StompFrameAbstract subclass based on the command.
     * Each subclass encapsulates the specific headers required for that command type.
     *
     * Only client-to-server commands are handled here (CONNECT, SEND, SUBSCRIBE,
     * UNSUBSCRIBE, DISCONNECT). Server-to-client commands (CONNECTED, MESSAGE,
     * RECEIPT, ERROR) are created directly by StompProtocol and ConnectionsImpl,
     * not decoded from incoming bytes.
     *
     * Throws IllegalArgumentException if required headers are missing.
     */
    private StompFrameAbstract createFrame(String command, ConcurrentHashMap<String, String> headers, String body) {
        switch (command) {
            case "CONNECT":
                // CONNECT requires login and passcode headers
                String user = headers.get("login");
                String passcode = headers.get("passcode");
                if (user == null || passcode == null) {throw new IllegalArgumentException("missing headers");}
                return new ConnectFrame(user, passcode);
            case "SEND":
                // SEND requires a destination header; the body is the message content
                String topic = headers.get("destination");
                if (topic == null) {throw new IllegalArgumentException("missing headers");}
                return new SendFrame(body, topic);
            case "SUBSCRIBE":
                // SUBSCRIBE requires a destination (channel) and an id (chosen by the client)
                String topic2 = headers.get("destination");
                String id2 = headers.get("id");
                if (topic2 == null || id2 == null) {throw new IllegalArgumentException("missing headers");}
                return new SubscribeFrame(topic2, Integer.parseInt(id2));
            case "UNSUBSCRIBE":
                // UNSUBSCRIBE requires the subscription id (the same id used in SUBSCRIBE)
                String id3 = headers.get("id");
                if (id3 == null) {throw new IllegalArgumentException("missing headers");}
                return new UnsubscribeFrame(Integer.parseInt(id3));
            case "DISCONNECT":
                // DISCONNECT requires a receipt id so the client can confirm logout
                String receiptid2 = headers.get("receipt");
                if (receiptid2 == null) {throw new IllegalArgumentException("missing headers");}
                return new DisconnectFrame(Integer.parseInt(receiptid2));
        }
        return null; // unknown command — StompProtocol will handle this as an error
    }

    /*
     * encode()
     * --------
     * Converts a StompFrameAbstract (server-to-client frame) into a byte array.
     * Delegates to the frame's own encode() method, which calls toString()
     * to produce the STOMP wire format string, then converts it to bytes.
     */
    @Override
    public byte[] encode(StompFrameAbstract message) {
        return message.encode();
    }
}
