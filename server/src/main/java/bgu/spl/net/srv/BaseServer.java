/*
 * BaseServer.java
 * ---------------
 * Abstract base class for the Thread-Per-Client (TPC) server mode.
 *
 * WHAT THIS CLASS DOES:
 *   - Opens a ServerSocket on the configured port.
 *   - Loops forever, calling accept() to wait for new client connections.
 *   - For each accepted connection, creates a BlockingConnectionHandler,
 *     assigns it a unique connectionId, registers it with ConnectionsImpl,
 *     and calls execute() to start handling it.
 *
 * WHY ABSTRACT?
 *   The only difference between "thread-per-client" and other models is HOW
 *   execute() works. In TPC, execute() spawns a new thread for each handler.
 *   This class provides everything else; the concrete subclass just implements execute().
 *
 * HOW IT FITS INTO THE SERVER:
 *   StompServer calls Server.threadPerClient(...) which creates a concrete subclass
 *   of BaseServer that implements execute() by spawning a new Thread.
 *   Alternatively, StompServer can use Server.reactor(...) to create a Reactor instead.
 *
 * FACTORIES:
 *   The constructor takes factory Suppliers for both the protocol and encoder/decoder.
 *   A new instance of each is created per connection (using Supplier.get()),
 *   ensuring each client has its own independent protocol and decoder state.
 */
package bgu.spl.net.srv;

import bgu.spl.net.api.MessageEncoderDecoder;
import bgu.spl.net.api.MessagingProtocol;
import java.io.IOException;
import java.net.ServerSocket;
import java.net.Socket;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.Supplier;

public abstract class BaseServer<T> implements Server<T> {

    private final int port;
    private final Supplier<MessagingProtocol<T>> protocolFactory;   // creates one StompProtocol per client
    private final Supplier<MessageEncoderDecoder<T>> encdecFactory; // creates one StompEncoderDecoder per client
    private ServerSocket sock;
    private ConnectionsImpl<T> connections; // shared state store for all clients
    private AtomicInteger connectionId = new AtomicInteger(1); // auto-incremented unique ID per connection

    public BaseServer(
            int port,
            Supplier<MessagingProtocol<T>> protocolFactory,
            Supplier<MessageEncoderDecoder<T>> encdecFactory) {

        this.port = port;
        this.protocolFactory = protocolFactory;
        this.encdecFactory = encdecFactory;
		this.sock = null;
        this.connections = new ConnectionsImpl<>(); // one shared ConnectionsImpl for the whole server
    }

    /*
     * serve()
     * -------
     * The main server loop. Opens the server socket and blocks on accept() forever.
     *
     * For each new client connection:
     *   1. Create a BlockingConnectionHandler with a fresh protocol and decoder.
     *   2. Assign the next available connectionId (atomic to avoid race conditions).
     *   3. Call protocol.start() so the protocol knows its ID and can access connections.
     *   4. Register the handler in ConnectionsImpl so other clients can send it messages.
     *   5. Call execute() — in TPC mode, this spawns a new thread for the handler.
     *
     * The loop exits when the thread is interrupted (e.g., server shutdown).
     */
    @Override
    public void serve() {

        try (ServerSocket serverSock = new ServerSocket(port)) {
			System.out.println("Server started");

            this.sock = serverSock; // store reference so close() can shut it down

            while (!Thread.currentThread().isInterrupted()) {

                Socket clientSock = serverSock.accept(); // BLOCKS until a client connects

                // Create fresh protocol and encoder/decoder instances for this client
                BlockingConnectionHandler<T> handler = new BlockingConnectionHandler<>(
                        clientSock,
                        encdecFactory.get(),
                        protocolFactory.get());
                int connectId = connectionId.getAndIncrement(); // atomically get and increment
                handler.getProtocol().start(connectId, connections); // give the protocol its ID
                connections.addConnection(connectId, handler);        // register in shared state
                execute(handler); // start handling this client (subclass decides how)
            }
        } catch (IOException ex) {
        }

        System.out.println("server closed!!!");
    }

    @Override
    public void close() throws IOException {
		if (sock != null)
			sock.close(); // closing the ServerSocket causes accept() to throw and the loop to exit
    }

    /*
     * execute()
     * ---------
     * Subclasses implement this to decide HOW to run the handler.
     * In TPC mode: new Thread(handler).start()
     * (The Reactor mode uses a different class entirely and does not extend BaseServer.)
     */
    protected abstract void execute(BlockingConnectionHandler<T>  handler);

}
