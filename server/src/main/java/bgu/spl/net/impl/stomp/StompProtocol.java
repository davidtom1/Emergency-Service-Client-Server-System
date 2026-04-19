/*
 * StompProtocol.java (SERVER SIDE)
 * ---------------------------------
 * This class handles all STOMP protocol logic for ONE connected client on the server.
 *
 * IMPORTANT: There is ONE StompProtocol instance per connected client.
 * All clients share the same ConnectionsImpl object (passed in via start()),
 * which is the central state store for the whole server.
 *
 * WHAT THIS CLASS DOES:
 *   When a client sends a STOMP frame to the server, the encoded bytes arrive at
 *   StompEncoderDecoder, which decodes them into a StompFrameAbstract object.
 *   That object is then passed to process() in this class.
 *
 *   process() reads the command and calls the appropriate private handler:
 *     CONNECT     -> connect()     : authenticate the user, send CONNECTED or ERROR
 *     SEND        -> send()        : broadcast a message to all channel subscribers
 *     SUBSCRIBE   -> subscribe()   : register this client as a subscriber to a channel
 *     UNSUBSCRIBE -> unsubscribe() : remove this client from a channel
 *     DISCONNECT  -> disconnect()  : gracefully log out the client
 *
 * THREAD SAFETY:
 *   Multiple clients may be active simultaneously (each on their own thread in TPC mode,
 *   or sharing a thread pool in Reactor mode). The shared state (ConnectionsImpl) uses
 *   ConcurrentHashMap and synchronized methods to handle concurrent access safely.
 *
 * LIFECYCLE:
 *   start()           -> called once when the client first connects; sets connectionId
 *   process(frame)    -> called for every complete STOMP frame received
 *   shouldTerminate() -> returns true after a DISCONNECT is processed; tells the server
 *                        framework to close this client's connection handler
 */
package bgu.spl.net.impl.stomp;

import java.util.concurrent.ConcurrentHashMap;

import bgu.spl.net.api.StompMessagingProtocol;
import bgu.spl.net.impl.stomp.Frame.ConnectFrame;
import bgu.spl.net.impl.stomp.Frame.ConnectedFrame;
import bgu.spl.net.impl.stomp.Frame.DisconnectFrame;
import bgu.spl.net.impl.stomp.Frame.ErrorFrame;
import bgu.spl.net.impl.stomp.Frame.MessageFrame;
import bgu.spl.net.impl.stomp.Frame.ReceiptFrame;
import bgu.spl.net.impl.stomp.Frame.SendFrame;
import bgu.spl.net.impl.stomp.Frame.StompFrameAbstract;
import bgu.spl.net.impl.stomp.Frame.SubscribeFrame;
import bgu.spl.net.impl.stomp.Frame.UnsubscribeFrame;
import bgu.spl.net.srv.ConnectionsImpl;

public class StompProtocol implements StompMessagingProtocol<StompFrameAbstract> {

    // When true, the server framework will stop reading from this client's socket.
    private boolean shouldTerminate = false;

    // Shared across ALL clients — holds usernames, passwords, subscriptions, etc.
    private ConnectionsImpl<StompFrameAbstract> connections;

    // Unique integer ID assigned to this client when they connect.
    // Used to look up and send frames to this specific client.
    private int connectionId;


    /*
     * start()
     * -------
     * Called by the server framework immediately after a new client connects.
     * Registers this protocol instance with the shared connections store.
     */
    @Override
    public void start(int connectionId, ConnectionsImpl<StompFrameAbstract> connections) {
        this.connectionId = connectionId;
        this.connections = connections;
    }

    /*
     * process()
     * ---------
     * Called once for every complete STOMP frame received from this client.
     * Dispatches to the correct private handler based on the frame's command.
     * Unknown commands result in an ERROR frame being sent back.
     */
    @Override
    public void process(StompFrameAbstract msg) {
        String command = msg.getCommand();
        switch (command) {
            case "CONNECT":
                connect((ConnectFrame) msg);
                break;
            case "SEND":
                send((SendFrame) msg);
                break;
            case "SUBSCRIBE":
                subscribe((SubscribeFrame) msg);
                break;
            case "UNSUBSCRIBE":
                unsubscribe((UnsubscribeFrame) msg);
                break;
            case "DISCONNECT":
                disconnect((DisconnectFrame) msg);
                break;
            default:
                connections.send(connectionId, new ErrorFrame("unknown command:" + command, connectionId, null, msg));
                break;

        }
    }

    /*
     * connect()
     * ---------
     * Handles the CONNECT frame: authenticates the user and sends back either
     * a CONNECTED frame (success) or an ERROR frame (failure).
     *
     * There are three possible outcomes:
     *
     *   Case 1 — New user (username not seen before):
     *     Register the username + password, mark as active, send CONNECTED.
     *
     *   Case 2 — Known user, wrong password:
     *     Send ERROR and disconnect. The client must reconnect with the correct password.
     *
     *   Case 3 — Known user, correct password, not currently logged in:
     *     Mark as active (re-login after a previous logout), send CONNECTED.
     *
     *   Case 4 (implicit) — Known user, correct password, already active:
     *     Send ERROR ("User already logged in") and disconnect.
     *     STOMP does not allow two simultaneous sessions for the same username.
     */
    private void connect(ConnectFrame frame) {
        String login = frame.getHeaders().get("login");
        String passcode = frame.getHeaders().get("passcode");
        if (login == null || passcode == null) {
            connections.send(connectionId, new ErrorFrame("Missing login or passcode", connectionId, null, frame));
            connections.disconnect(connectionId);
            return;
        }

        // checkUser returns the stored password for this username, or null if the username is new.
        String password = connections.checkUser(login);
        if (password == null) {
            // Case 1: First time this username is seen — register and log in
            connections.addUser(login, passcode);
            connections.addActiveUser(connectionId, login);
            connections.send(connectionId, new ConnectedFrame());
        } else if (!passcode.equals(password)) {
            // Case 2: Known user but wrong password
            connections.send(connectionId, new ErrorFrame("Wrong password", connectionId, null, frame));
            connections.disconnect(connectionId);
            return;
        } else {
            // Case 3 or 4: Correct password — check if user already has an active session
            // connectedUser() returns the username if currently logged in, null if not.
            if (connections.connectedUser(login) == null) {
                // Case 3: Not currently logged in — allow re-login
                connections.addActiveUser(connectionId, login);
                connections.send(connectionId, new ConnectedFrame());
            } else {
                // Case 4: Already has an active session — reject
                connections.send(connectionId, new ErrorFrame("User already logged in", connectionId, null, frame));
                connections.disconnect(connectionId);
                return;
            }
        }
    }

    /*
     * send()
     * ------
     * Handles the SEND frame: broadcasts the message body to ALL clients
     * currently subscribed to the destination channel.
     *
     * Each subscriber gets a MESSAGE frame with:
     *   - their own unique subscription ID (so their client can match it to their subscription)
     *   - the destination channel name
     *   - the original message body
     *
     * How broadcasting works:
     *   1. Look up the destination channel in channelSubscribers to get a set of connectionIds.
     *   2. For each connectionId in that set, look up their subscription ID for this channel.
     *   3. Build a MessageFrame with that subscriber's subId and send it.
     *
     * If send() returns false, the subscriber's connection has died — disconnect them.
     * A NullPointerException means the channel has no subscribers yet — silently skip.
     */
    private void send(SendFrame frame) {
        String dest = frame.getHeaders().get("destination");
        if (dest == null) {
            ErrorFrame error = new ErrorFrame("Missing destination", connectionId, null, frame);
            connections.send(connectionId, error);
            connections.disconnect(connectionId);
            return;
        }

        // Get the map of channel -> set of subscriber connectionIds
        ConcurrentHashMap<String, ConcurrentHashMap<Integer, Boolean>> channelSubscribers = connections.getChannelSub();
        // Get the map of connectionId -> (channel -> subId), used to find each subscriber's subId
        ConcurrentHashMap<Integer, ConcurrentHashMap<String, Integer>> userSubscriptions = connections.getSub();
        try {
            // Get the set of connectionIds subscribed to this destination channel
            ConcurrentHashMap<Integer, Boolean> destSub = channelSubscribers.get(dest);
            if (!destSub.isEmpty()) {
                for (Integer id : destSub.keySet()) {
                    // For each subscriber, find their specific subscription ID for this channel
                    ConcurrentHashMap<String, Integer> userSub = userSubscriptions.get(id);
                   if (!userSub.isEmpty()) {
                    int subId = userSub.get(dest);
                    // Build a MESSAGE frame tagged with this subscriber's subId and send it
                    MessageFrame broadcast = new MessageFrame(subId, dest, frame.getBody());
                        if (!connections.send(id, broadcast)) {
                            // Failed to send — the subscriber's connection is broken; disconnect them
                            connections.send(id, new ErrorFrame("couldn't send message - connection terminated", connectionId, null, frame));
                            connections.disconnect(id);
                        }
                   }
                }
            }
        } catch (NullPointerException e) {
            // dest has no subscribers yet — nothing to broadcast, silently skip
        }

    }

    /*
     * subscribe()
     * -----------
     * Handles the SUBSCRIBE frame: registers this client as a subscriber to a channel.
     *
     * The frame must include:
     *   - destination : the channel name (e.g., "/topic/ambulance")
     *   - id          : a numeric subscription ID chosen by the client
     *                   (used later in UNSUBSCRIBE and in MESSAGE frames sent to this client)
     *
     * Delegates the actual data structure update to ConnectionsImpl.subscribe().
     * Returns an error if the client is already subscribed to this channel.
     */
    private void subscribe(SubscribeFrame frame) {
        String dest = frame.getHeaders().get("destination");
        String subIdStr = frame.getHeaders().get("id");

        if (dest == null || subIdStr == null) {
            connections.send(connectionId, new ErrorFrame("Missing destination or id", connectionId, null, frame));
            connections.disconnect(connectionId);
            return;
        }

        int subId;
        try {
            subId = Integer.valueOf(subIdStr);
        } catch (NumberFormatException e) {
            connections.send(connectionId, new ErrorFrame("Invalid subscription id", connectionId, null, frame));
            connections.disconnect(connectionId);
            return;
        }

        // connections.subscribe() returns false if the client is already subscribed to this channel
        if (!connections.subscribe(connectionId, dest, subId)) {
            connections.send(connectionId, new ErrorFrame("User already subscribed to this channel", connectionId, null, frame));
            return;
        }

    }

    /*
     * unsubscribe()
     * -------------
     * Handles the UNSUBSCRIBE frame: removes this client from a channel's subscriber list.
     *
     * The frame must include:
     *   - id : the numeric subscription ID (the same one used in the original SUBSCRIBE)
     *
     * Note: the frame uses the subscription ID, not the channel name, to identify
     * which subscription to remove. ConnectionsImpl.unsubscribe() does the reverse
     * lookup to find the channel name from the subscription ID.
     */
    private void unsubscribe(UnsubscribeFrame frame) {
        String subIdStr = frame.getHeaders().get("id");
        if (subIdStr == null) {
            connections.send(connectionId, new ErrorFrame("Missing id", connectionId, null, frame));
            connections.disconnect(connectionId);
            return;
        }

        int subId;
        try {
            subId = Integer.valueOf(subIdStr);
        } catch (NumberFormatException e) {
            connections.send(connectionId, new ErrorFrame("Invalid subscription id", connectionId, null, frame));
            connections.disconnect(connectionId);
            return;
        }

        // connections.unsubscribe() returns false if no matching subscription was found
        if (!connections.unsubscribe(connectionId, subId)) {
            connections.send(connectionId, new ErrorFrame("Not subscribe to channel", connectionId, null, frame));
        }
    }

    /*
     * disconnect()
     * ------------
     * Handles the DISCONNECT frame: gracefully logs out this client.
     *
     * The DISCONNECT frame includes a "receipt" header with a numeric ID.
     * The server must reply with a RECEIPT frame containing that same ID
     * BEFORE closing the connection, so the client knows the logout was acknowledged.
     *
     * Steps:
     *   1. Parse the receipt ID from the frame.
     *   2. Send a RECEIPT frame back to the client (confirming logout).
     *   3. Remove this client from the active connections (frees subscriptions, etc.).
     *   4. Set shouldTerminate = true so the server framework closes the socket.
     */
    private void disconnect(DisconnectFrame frame) {
        String receiptIdStr = frame.getHeaders().get("receipt");
        if (receiptIdStr == null) {
            connections.send(connectionId, new ErrorFrame("Missing receipt id", connectionId, null, frame));
            connections.disconnect(connectionId);
            return;
        }

        int receiptId;
        try {
            receiptId = Integer.valueOf(receiptIdStr);
            // Send RECEIPT before disconnecting so the client receives the confirmation
            connections.send(connectionId, new ReceiptFrame(receiptId));
            // Remove this client's active session and all their subscriptions
            connections.disconnect(connectionId);
            // Tell the server framework to stop reading from this client's socket
            this.shouldTerminate = true;
        } catch (NumberFormatException e) {
            connections.send(connectionId, new ErrorFrame("Invalid receipt id", connectionId, null, frame));
        }
    }

    /*
     * shouldTerminate()
     * -----------------
     * Called by the server framework after every process() call.
     * Returns true only after a DISCONNECT is processed, signaling that this
     * client's connection handler should stop and the socket should be closed.
     */
    @Override
    public boolean shouldTerminate() {
        return shouldTerminate;
    }


}
