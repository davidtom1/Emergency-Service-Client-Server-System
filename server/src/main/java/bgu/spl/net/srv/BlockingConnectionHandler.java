/*
 * BlockingConnectionHandler.java
 * --------------------------------
 * Handles one connected client in THREAD-PER-CLIENT (TPC) mode.
 *
 * In TPC mode, each accepted TCP connection gets its own dedicated Java thread
 * (created by BaseServer.execute()). That thread runs this class's run() method
 * and blocks on reading until the client sends data or disconnects.
 *
 * WHAT THIS CLASS DOES:
 *   - Reads raw bytes from the client's socket, one byte at a time.
 *   - Feeds each byte into the StompEncoderDecoder, which accumulates bytes
 *     until a complete STOMP frame is assembled (terminated by '\0').
 *   - When a complete frame is ready, passes it to StompProtocol.process().
 *   - Sends frames BACK to the client via send() (called by ConnectionsImpl.send()).
 *   - Stops when StompProtocol.shouldTerminate() returns true (after DISCONNECT),
 *     or when the client drops the connection (read returns -1).
 *
 * IMPLEMENTS:
 *   - Runnable         : so it can be run on its own thread
 *   - ConnectionHandler: so ConnectionsImpl can call send() on it to push frames out
 */
package bgu.spl.net.srv;

import bgu.spl.net.api.MessageEncoderDecoder;
import bgu.spl.net.api.MessagingProtocol;
import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.IOException;
import java.net.Socket;

public class BlockingConnectionHandler<T> implements Runnable, ConnectionHandler<T> {

    private final MessagingProtocol<T> protocol;  // handles the STOMP protocol logic
    private final MessageEncoderDecoder<T> encdec; // decodes bytes into frames, encodes frames into bytes
    private final Socket sock;                     // the raw TCP socket for this client
    private BufferedInputStream in;                // buffered reader for incoming bytes
    private BufferedOutputStream out;              // buffered writer for outgoing bytes
    private volatile boolean connected = true;     // set to false to stop the read loop from outside

    public BlockingConnectionHandler(Socket sock, MessageEncoderDecoder<T> reader, MessagingProtocol<T> protocol) {
        this.sock = sock;
        this.encdec = reader;
        this.protocol = protocol;
    }

    /*
     * run()
     * -----
     * The main loop for this client's dedicated thread.
     *
     * Reads one byte at a time from the socket and feeds it to decodeNextByte().
     * decodeNextByte() returns null while it's still accumulating a frame,
     * and returns the complete frame object when it detects the '\0' terminator.
     *
     * When a frame is received, it's passed to protocol.process().
     * After each process() call, we check shouldTerminate() — if the client sent
     * a DISCONNECT, shouldTerminate() returns true and we exit the loop.
     *
     * The socket is automatically closed when the try-with-resources block exits.
     */
    @Override
    public void run() {
        try (Socket sock = this.sock) { // try-with-resources ensures socket is closed on exit
            int read;

            in = new BufferedInputStream(sock.getInputStream());
            out = new BufferedOutputStream(sock.getOutputStream());

            // Loop: read one byte at a time, stop if:
            //   - protocol.shouldTerminate() -> client sent DISCONNECT
            //   - !connected               -> close() was called externally
            //   - read < 0                 -> client dropped the connection (EOF)
            while (!protocol.shouldTerminate() && connected && (read = in.read()) >= 0) {
                T nextMessage = encdec.decodeNextByte((byte) read); // accumulate byte; returns frame when complete
                if (nextMessage != null) {
                    protocol.process(nextMessage); // handle the complete frame
                }
            }

        } catch (IOException ex) {
            ex.printStackTrace();
        }

    }

    /*
     * close()
     * -------
     * Called externally (e.g., by ConnectionsImpl.disconnect()) to force-stop
     * this handler. Sets connected = false to exit the read loop, then closes the socket.
     */
    @Override
    public void close() throws IOException {
        connected = false;
        sock.close();
    }

    /*
     * send()
     * ------
     * Called by ConnectionsImpl.send() to push a frame out to this client.
     * Encodes the frame object into bytes and writes them to the output stream.
     * Flushes immediately so the client receives the data without waiting for the buffer to fill.
     */
    @Override
    public void send(T msg) {
        try {
            byte[] bytes = encdec.encode(msg);
            out.write(bytes);
            out.flush(); // flush so the bytes are sent immediately
        } catch (IOException e) {
            e.printStackTrace();
        }
    }

    public MessagingProtocol<T> getProtocol() {
        return this.protocol;
    }

}
