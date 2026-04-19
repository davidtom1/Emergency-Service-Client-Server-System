/*
 * ConnectionsImpl.java
 * --------------------
 * This is the CENTRAL STATE STORE for the entire server.
 * There is exactly ONE instance of this class, shared by ALL connected clients.
 *
 * It keeps track of:
 *   1. User credentials (username -> password)
 *   2. Active sessions (connectionId -> username + connection handler)
 *   3. Channel subscriptions (channel -> set of connectionIds)
 *   4. Each user's subscription IDs (connectionId -> channel -> subId)
 *
 * THREAD SAFETY:
 *   Many clients may be connected simultaneously and their threads may all call
 *   methods here at the same time. Methods that modify shared state are
 *   marked synchronized. The maps themselves are ConcurrentHashMaps,
 *   which are safe for concurrent reads but may still need synchronization
 *   for compound operations (e.g., check-then-insert sequences).
 *
 * DATA STRUCTURES EXPLAINED:
 *
 *   loginInfo:
 *     Maps username -> password.
 *     Populated when a new user connects for the first time.
 *     Never cleared — passwords persist for the lifetime of the server.
 *
 *   activeUsers:
 *     Maps connectionId -> (username, ConnectionHandler).
 *     A connection is added here when a TCP connection is accepted,
 *     and updated with the username when the user successfully logs in.
 *     Removed when the user disconnects.
 *
 *   channelSubscribers:
 *     Maps channel name -> set of connectionIds subscribed to that channel.
 *     Used by send() to find who to broadcast a message to.
 *     The Boolean value in the inner map is unused (the map is used as a Set).
 *
 *   userSubscriptions:
 *     Maps connectionId -> (channel name -> subscription ID).
 *     Used to look up the correct subId when building MESSAGE frames for a subscriber.
 *     Also used for reverse lookup during UNSUBSCRIBE (find channel name from subId).
 */
package bgu.spl.net.srv;
import java.util.AbstractMap.SimpleEntry;
import java.util.concurrent.ConcurrentHashMap;

public class ConnectionsImpl<T> implements Connections<T> {

    // username -> password (persists across login/logout cycles)
    private ConcurrentHashMap<String, String> loginInfo;

    // connectionId -> (username, ConnectionHandler)
    // The username is null until the user successfully logs in via CONNECT.
    private ConcurrentHashMap<Integer, SimpleEntry<String, ConnectionHandler<T>>> activeUsers;

    // channel name -> set of connectionIds (using ConcurrentHashMap as a Set with Boolean values)
    private ConcurrentHashMap<String, ConcurrentHashMap<Integer, Boolean>> channelSubscribers;

    // connectionId -> (channel name -> subscription ID)
    private ConcurrentHashMap<Integer, ConcurrentHashMap<String, Integer>> userSubscriptions;

    public ConnectionsImpl() {
        this.loginInfo = new ConcurrentHashMap<>();
        this.activeUsers = new ConcurrentHashMap<>();
        this.channelSubscribers = new ConcurrentHashMap<>();
        this.userSubscriptions = new ConcurrentHashMap<>();
    }

    /*
     * addConnection()
     * ---------------
     * Called when a new TCP connection is accepted (before any CONNECT frame arrives).
     * Registers the connection handler under the given connectionId.
     * The username in the entry is null at this point — it gets filled in by
     * addActiveUser() after the CONNECT frame is processed successfully.
     */
   public synchronized void addConnection(int connectionId, ConnectionHandler<T> handler) {
        if (handler != null) {
            // Username is null until the user authenticates with a CONNECT frame
            activeUsers.put(connectionId, new SimpleEntry<>(null, handler));
        }
        else {
            throw new IllegalArgumentException("Handler cannot be null");
        }
    }

    /*
     * send()
     * ------
     * Sends a message to a specific client identified by connectionId.
     * Looks up the ConnectionHandler for that connection and calls handler.send().
     * Returns true if the message was sent successfully, false if:
     *   - No active connection exists for that connectionId
     *   - The handler.send() threw an exception (e.g., socket was closed)
     */
    @Override
    public boolean send(int connectionId, T msg) {
        if (activeUsers.containsKey(connectionId)) {
            ConnectionHandler<T> handler = activeUsers.get(connectionId).getValue();
            try {
                handler.send(msg);
                return true;
            } catch (Exception e) {
                return false;
            }
        }
        else {
            return false;
        }

    }

    /*
     * sendAllSub()
     * ------------
     * Was intended to send a message to all subscribers of a channel.
     * Not used — the broadcasting logic lives in StompProtocol.send() instead,
     * because it needs to customize the MESSAGE frame per subscriber (different subId).
     */
    @Override
    public void sendAllSub(String channel, T msg) {
        return; // chose not to use this function at all
    }

    /*
     * disconnect()
     * ------------
     * Removes a client completely from the server's state.
     * Called when a client sends DISCONNECT or when their connection is broken.
     *
     * Steps:
     *   1. Remove from activeUsers (client is no longer reachable).
     *   2. Remove from userSubscriptions (clean up their subscription ID mappings).
     *   3. Remove their connectionId from every channel in channelSubscribers
     *      (so they no longer receive MESSAGE frames).
     */
    @Override
    public synchronized void disconnect(int connectionId) {
        if (activeUsers.containsKey(connectionId)) {
            activeUsers.remove(connectionId);
        }
        userSubscriptions.remove(connectionId);
        // Remove this connectionId from every channel's subscriber set
        channelSubscribers.values().forEach(channel -> channel.remove(connectionId));
    }

    /*
     * subscribe()
     * -----------
     * Registers a client as a subscriber to a channel.
     * Returns false if the client is already subscribed to that channel (duplicate).
     *
     * Steps:
     *   1. Verify the client is active (has a valid session).
     *   2. Create the channel entry in channelSubscribers if it doesn't exist yet.
     *   3. Create the subscription map for this client if it doesn't exist yet.
     *   4. Check for duplicate subscription (return false if already subscribed).
     *   5. Add the connectionId to the channel's subscriber set.
     *   6. Record the (channel -> subId) mapping for this client.
     */
    public synchronized boolean subscribe(int connectionId, String channel, int subId) {
        if (activeUsers.containsKey(connectionId)) { // client must be active to subscribe
            // Create channel entry if this is the first subscriber
            channelSubscribers.putIfAbsent(channel, new ConcurrentHashMap<>());
            // Create subscription map for this client if they have no subscriptions yet
            userSubscriptions.putIfAbsent(connectionId, new ConcurrentHashMap<>());

            ConcurrentHashMap<String, Integer> userSub = userSubscriptions.get(connectionId);
            if (userSub.containsKey(channel)) {
                // Client is already subscribed to this channel — reject
                return false;
            }

            channelSubscribers.get(channel).putIfAbsent(connectionId, true); // add to channel's subscriber set
            userSub.put(channel, subId);  // record which subId this client uses for this channel
            return true;
        }
        return false;
    }

    /*
     * unsubscribe()
     * -------------
     * Removes a client's subscription to a channel, identified by subscription ID.
     *
     * Note: the UNSUBSCRIBE frame provides the subId, NOT the channel name.
     * So we must do a reverse lookup: iterate over this client's subscriptions
     * to find which channel corresponds to the given subId.
     *
     * Once found, we remove the channel from:
     *   - userSubscriptions (this client's subscription map)
     *   - channelSubscribers (the channel's subscriber set)
     *
     * Returns false if no matching subscription was found.
     */
    public synchronized boolean unsubscribe(int connectionId, int subId) {
        ConcurrentHashMap<String, Integer> subscriptions = userSubscriptions.get(connectionId);
        if (subscriptions == null) {
            return false; // this client has no subscriptions at all
        }
        // Reverse lookup: find the channel name that maps to the given subId
        String channel = null;
        for (String s : subscriptions.keySet()) {
            if (subscriptions.get(s) == subId) {
                channel = s;
                break;
            }
        }

        if (channel != null) {
            // Found the channel — remove from both data structures
            userSubscriptions.get(connectionId).remove(channel);
            ConcurrentHashMap<Integer, Boolean> subscribers = channelSubscribers.get(channel);
            if (subscribers != null) {
                subscribers.remove(connectionId);
            }
            return true;
        }
       return false; // no subscription matched the given subId
    }

    /*
     * checkUser()
     * -----------
     * Returns the stored password for the given username, or null if the username
     * has never been registered. Used during CONNECT to decide whether this is a
     * new user or a returning user.
     */
    public synchronized String checkUser(String user) {
        if (loginInfo.containsKey(user))
          return loginInfo.get(user);
        return null;
    }

    /*
     * addUser()
     * ---------
     * Registers a new username and password. Called once per unique username
     * the first time that user successfully connects.
     */
    public void addUser(String username, String password) {
        loginInfo.put(username, password);
    }

    /*
     * connectedUser()
     * ---------------
     * Searches the active sessions to see if the given username is currently
     * logged in (has an active session).
     * Returns the username if found, or null if the user is not currently logged in.
     * Used during CONNECT to prevent two simultaneous sessions for the same username.
     */
    public synchronized String connectedUser(String login) {
        for (Integer i: activeUsers.keySet()) {
            if (activeUsers.get(i).getKey() != null) {
                if (activeUsers.get(i).getKey().equals(login)) {
                    return login;

                }

            }
        }
        return null;
    }

    /*
     * addActiveUser()
     * ---------------
     * Associates a username with an existing connection entry in activeUsers.
     * Called after a successful CONNECT to link the connectionId with a username.
     *
     * Why do we need to replace the entry instead of just updating a field?
     * SimpleEntry is immutable once created, so we must create a new entry
     * with the same ConnectionHandler but the newly-known username.
     */
    public synchronized void addActiveUser(int connectionId, String user) {
        SimpleEntry<String, ConnectionHandler<T>> entry = this.activeUsers.get(connectionId);
        if (entry != null) {
            // Create a new entry keeping the same handler but adding the username
            SimpleEntry<String, ConnectionHandler<T>> updatedEntry = new SimpleEntry<>(user, entry.getValue());
            activeUsers.remove(connectionId);
            activeUsers.put(connectionId, updatedEntry);
        }
    }

    /*
     * getChannelSub() / getSub()
     * --------------------------
     * Expose the raw maps to StompProtocol so it can do the broadcast logic.
     * These are package-visible helpers (no access modifier = package-private by convention,
     * though technically public here due to interface requirements).
     */
    public ConcurrentHashMap<String, ConcurrentHashMap<Integer, Boolean>> getChannelSub() {
        return this.channelSubscribers;
    }

    public ConcurrentHashMap<Integer, ConcurrentHashMap<String, Integer>> getSub() {
        return this.userSubscriptions;
    }


}
