#pragma once
#ifdef ESP32

// THROWAWAY MEASUREMENT CODE, not meant for a pull request.
// A minimal TLS listener on port 443 next to the normal web server, to measure what HTTPS
// would cost on this board: handshake time, how long loop() is blocked, heap per session,
// stack use of the loop task and transfer speed. Call from the main loop.
void tlsSpikeLoop();

#endif
