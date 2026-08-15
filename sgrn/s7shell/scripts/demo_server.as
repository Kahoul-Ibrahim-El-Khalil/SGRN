// Create a shared runtime and load a schema
PlcRuntime@ rt = PlcRuntime("schema.scl");

// Start an S7 Server on port 1024, sharing the runtime
S7Server@ server = S7Server(rt, "127.0.0.1", 1024);
server.start();

print("S7Server running. Clients: " + server.clientsCount() + "\n");

// Connect a client to the server, sharing the same runtime
S7Client@ client = S7Client("127.0.0.1", 0, 1, 1024, rt);

// Interact with the shared memory
// db(1) resolves from the runtime
auto@ data = client.db(1);
data.put(); // Push to the server (dirty regions will be tracked in rt)

print("Client connected to S7Server.\n");
